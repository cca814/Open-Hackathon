#ifndef _INGRESS_P4_
#define _INGRESS_P4_

#include "include/const.p4"
#include "include/header.p4"

control MyIngress(inout headers_t headers,
                  inout metadata_t metadata,
                  inout standard_metadata_t standard_metadata) {
    // A readable data-plane counter needs a register (counter externs cannot
    // be read by P4). One outstanding Send Only PSN for the one fixed group.
    // [63] valid, [62] resend aggregate ACK, [61:38] sender PSN,
    // [37:32] ACK count, [31:0] members already counted.
    register<bit<64>>(1) multicast_state;

    action drop() {
        mark_to_drop(standard_metadata);
    }

    // Pair access ports like an Ethernet bridge. Normal traffic is never NATed.
    // RoCE still requires a matching MITM connection below.
    action forward(PORT_T port) {
        standard_metadata.egress_spec = port;
    }

    table forwarding {
        key = {
            standard_metadata.ingress_port: exact;
        }
        actions = {
            forward;
            drop;
        }
        size = 512;
        const default_action = drop();
    }

    action translate_connection(MAC_ADDR_T src_mac,
                                MAC_ADDR_T dst_mac,
                                IPV4_ADDR_T src_ip,
                                IPV4_ADDR_T dst_ip,
                                bit<16> udp_src_port,
                                QP_T dst_qp,
                                bit<16> p_key,
                                bit<24> psn_delta,
                                bit<24> msn_delta,
                                PORT_T port) {
        metadata.connection_matched = 1;

        if (headers.ipv4.src_addr != src_ip ||
            headers.ipv4.dst_addr != dst_ip ||
            headers.udp.src_port != udp_src_port ||
            headers.bth.dest_qp != dst_qp ||
            headers.bth.p_key != p_key ||
            psn_delta != 0) {
            metadata.icrc_dirty = 1;
        }

        headers.ethernet.src_addr = src_mac;
        headers.ethernet.dst_addr = dst_mac;
        headers.ipv4.src_addr = src_ip;
        headers.ipv4.dst_addr = dst_ip;
        headers.udp.src_port = udp_src_port;
        headers.bth.dest_qp = dst_qp;
        headers.bth.p_key = p_key;

        headers.bth.psn = headers.bth.psn + psn_delta;
        if (headers.aeth.isValid()) {
            if (msn_delta != 0) {
                metadata.icrc_dirty = 1;
            }
            headers.aeth.msn = headers.aeth.msn + msn_delta;
        }

        headers.ipv4.ttl = headers.ipv4.ttl - 1;
        headers.udp.checksum = 0;
        standard_metadata.egress_spec = port;
    }

    action multicast_send(bit<6> member_count, bit<24> initial_psn) {
        bit<64> state;
        bit<24> next_psn;
        bool accepted = false;

        if (headers.bth.opcode == BTH_OP_RC_SEND_ONLY &&
            member_count > 0 && member_count <= MAX_MULTICAST_MEMBERS) {
            @atomic {
                multicast_state.read(state, 0);
                next_psn = state[61:38] + 24w1;
                if (state[63:63] == 0) {
                    if (headers.bth.psn == initial_psn) {
                        state = 1w1 ++ 1w0 ++ headers.bth.psn ++ 6w0 ++ 32w0;
                        accepted = true;
                    }
                } else if (headers.bth.psn == state[61:38]) {
                    // Retransmission must retain the partial count/bitmap.
                    // If the aggregate ACK was lost, allow one replay ACK.
                    if (state[37:32] == member_count) {
                        state[62:62] = 1;
                    }
                    accepted = true;
                } else if (state[37:32] == member_count &&
                           headers.bth.psn == next_psn) {
                    state = 1w1 ++ 1w0 ++ headers.bth.psn ++ 6w0 ++ 32w0;
                    accepted = true;
                }
                if (accepted) {
                    multicast_state.write(0, state);
                }
            }
        }

        if (accepted) {
            metadata.multicast_group = MULTICAST_GROUP;
            standard_metadata.mcast_grp = MULTICAST_GROUP;
            standard_metadata.egress_spec = CPU_PORT;
            // Every member must ACK this packet, even if the sender batches
            // ACK requests. ICRC is repaired after each replica is rewritten.
            headers.bth.a_req = 1;
        } else {
            drop();
        }
    }

    action multicast_ack(bit<5> member_id,
                         bit<6> member_count,
                         bit<24> initial_psn,
                         MAC_ADDR_T src_mac,
                         MAC_ADDR_T dst_mac,
                         IPV4_ADDR_T src_ip,
                         IPV4_ADDR_T dst_ip,
                         bit<16> udp_src_port,
                         QP_T dst_qp,
                         bit<16> p_key,
                         bit<24> psn_delta,
                         PORT_T port) {
        bit<64> state;
        bit<24> sender_psn;
        bit<32> member_bit;
        bit<6> count;
        bool release = false;
        bool success = false;

        sender_psn = headers.bth.psn + psn_delta;
        member_bit = 32w1 << member_id;
        if (headers.bth.opcode == BTH_OP_RC_ACK &&
            headers.aeth.isValid() && member_count > 0 &&
            member_count <= MAX_MULTICAST_MEMBERS &&
            (bit<6>)member_id < member_count) {
            @atomic {
                multicast_state.read(state, 0);
                if (state[63:63] == 1 && state[61:38] == sender_psn) {
                    if ((headers.aeth.syndrome & 8w0xe0) == 0) {
                        success = true;
                        count = state[37:32];
                        if ((state[31:0] & member_bit) == 0) {
                            state[31:0] = state[31:0] | member_bit;
                            count = count + 1;
                            state[37:32] = count;
                            if (count == member_count) {
                                release = true;
                            }
                        } else if (count == member_count && state[62:62] == 1) {
                            release = true;
                        }
                        if (release) {
                            state[62:62] = 0;
                        }
                        multicast_state.write(0, state);
                    } else if (((headers.aeth.syndrome & 8w0xe0) == 8w0x20 ||
                                (headers.aeth.syndrome & 8w0xe0) == 8w0x60) &&
                               (state[31:0] & member_bit) == 0) {
                        // RNR/NAK is not a successful ACK. Preserve it so the
                        // sender can retry/fail; never reset successful peers.
                        release = true;
                    }
                }
            }
        }

        if (release) {
            translate_connection(src_mac, dst_mac, src_ip, dst_ip,
                                 udp_src_port, dst_qp, p_key, psn_delta, 0, port);
            if (success) {
                // One Send Only packet per message, QPs start with MSN zero.
                // No member's individual receive credits represent the group.
                headers.aeth.syndrome = 0x1f; // ACK, unlimited/invalid credits
                headers.aeth.msn = sender_psn - initial_psn + 24w1;
                metadata.icrc_dirty = 1;
            }
        } else {
            drop();
        }
    }

    // Controller-installed persistent connection information.
    table connections {
        key = {
            standard_metadata.ingress_port: exact;
            headers.ipv4.src_addr: exact;
            headers.ipv4.dst_addr: exact;
            headers.bth.dest_qp: exact;
            headers.bth.opcode: exact;
        }
        actions = {
            translate_connection;
            multicast_send;
            multicast_ack;
            drop;
        }
        size = CONNECTION_TABLE_SIZE;
        const default_action = drop();
    }

    action request_icrc_repair() {
        headers.packet_in.setValid();
        headers.packet_in.egress_port =
            (bit<16>)standard_metadata.egress_spec;
        standard_metadata.egress_spec = CPU_PORT;
    }

    apply {
        metadata.connection_matched = 0;
        metadata.icrc_dirty = 0;
        metadata.multicast_group = 0;
        standard_metadata.mcast_grp = 0;
        headers.packet_in.setInvalid();

        if (standard_metadata.parser_error != error.NoError ||
            standard_metadata.checksum_error != 0 ||
            !headers.ethernet.isValid()) {
            drop();
            return;
        }

        // Validate IPv4 length before bridging; TCP/ICMP and IP options remain
        // unparsed payload and are emitted unchanged, preserving their checksums.
        if (headers.ipv4.isValid()) {
            if (headers.ipv4.version != 4 || headers.ipv4.ihl < 5 ||
                headers.ipv4.total_len < (bit<16>)headers.ipv4.ihl * 16w4) {
                drop();
                return;
            }
            if (!headers.packet_out.isValid() &&
                standard_metadata.packet_length <
                    32w14 + (bit<32>)headers.ipv4.total_len) {
                drop();
                return;
            }
        }

        // CPU replies take the original, validated RoCE reinjection path below.
        if (!headers.packet_out.isValid()) {
            if (standard_metadata.ingress_port == CPU_PORT) {
                drop();
                return;
            }
            // The translation path does not reassemble fragments or parse IP
            // options, so it cannot safely classify these UDP datagrams.
            if (headers.ipv4.isValid() && headers.ipv4.protocol == IP_PROTO_UDP &&
                (headers.ipv4.ihl != 5 || headers.ipv4.frag_offset != 0 ||
                 (headers.ipv4.flags & 3w1) != 0)) {
                drop();
                return;
            }
            // Only ordinary traffic needs a one-to-one bridge mapping. RoCE
            // has its own connection/group lookup, including multiple QPs on
            // the same physical port.
            if (!headers.ipv4.isValid() || headers.ipv4.protocol != IP_PROTO_UDP ||
                (headers.udp.isValid() && headers.udp.dst_port != UDP_PORT_ROCEV2)) {
                forwarding.apply();
                if (standard_metadata.egress_spec == CPU_PORT ||
                    standard_metadata.egress_spec == DROP_PORT ||
                    standard_metadata.egress_spec == standard_metadata.ingress_port) {
                    drop();
                }
                return;
            }
        }

        if (!headers.ipv4.isValid() ||
            !headers.udp.isValid() ||
            !headers.bth.isValid()) {
            drop();
            return;
        }

        if (headers.ipv4.version != 4 || headers.ipv4.ihl != 5 ||
            headers.ipv4.frag_offset != 0 ||
            (headers.ipv4.flags & 3w1) != 0 ||
            headers.ipv4.protocol != IP_PROTO_UDP ||
            headers.udp.dst_port != UDP_PORT_ROCEV2 ||
            headers.bth.transport_header_version != 0 ||
            (headers.bth.opcode != BTH_OP_RC_SEND_ONLY &&
             headers.bth.opcode != BTH_OP_RC_ACK)) {
            drop();
            return;
        }

        if (headers.bth.opcode == BTH_OP_RC_ACK &&
            (!headers.aeth.isValid() || headers.udp.len != 28 ||
             headers.bth.pad_count != 0)) {
            drop();
            return;
        }

        if (headers.packet_out.isValid()) {
            if (standard_metadata.ingress_port != CPU_PORT ||
                standard_metadata.packet_length <
                    32w16 + (bit<32>)headers.ipv4.total_len ||
                headers.packet_out.egress_port >= (bit<16>)DROP_PORT ||
                headers.packet_out.egress_port == (bit<16>)CPU_PORT) {
                drop();
                return;
            }
            standard_metadata.egress_spec =
                (PORT_T)headers.packet_out.egress_port;
            headers.packet_out.setInvalid();
            return;
        }

        if (standard_metadata.ingress_port == CPU_PORT ||
            standard_metadata.packet_length <
                32w14 + (bit<32>)headers.ipv4.total_len ||
            headers.ipv4.ttl <= 1) {
            drop();
            return;
        }

        connections.apply();
        if (metadata.multicast_group == MULTICAST_GROUP) {
            return;
        }
        if (metadata.connection_matched == 0) {
            return;
        }
        if (standard_metadata.egress_spec == CPU_PORT ||
            standard_metadata.egress_spec == DROP_PORT) {
            drop();
            return;
        }

        if (metadata.icrc_dirty == 1) {
            request_icrc_repair();
        }
    }
}

#endif
