#ifndef _INGRESS_P4_
#define _INGRESS_P4_

#include "include/const.p4"
#include "include/header.p4"

control MyIngress(inout headers_t headers,
                  inout metadata_t metadata,
                  inout standard_metadata_t standard_metadata) {
    action drop() {
        mark_to_drop(standard_metadata);
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
        headers.packet_in.setInvalid();

        if (standard_metadata.parser_error != error.NoError ||
            standard_metadata.checksum_error != 0 ||
            !headers.ethernet.isValid() ||
            !headers.ipv4.isValid() ||
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
