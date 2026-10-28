#ifndef _EGRESS_P4_
#define _EGRESS_P4_

#include "include/const.p4"
#include "include/header.p4"

control MyEgress(inout headers_t headers,
                 inout metadata_t metadata,
                 inout standard_metadata_t standard_metadata) {
    action drop() {
        mark_to_drop(standard_metadata);
    }

    action multicast_member(MAC_ADDR_T src_mac,
                            MAC_ADDR_T dst_mac,
                            IPV4_ADDR_T src_ip,
                            IPV4_ADDR_T dst_ip,
                            bit<16> udp_src_port,
                            QP_T dst_qp,
                            bit<16> p_key,
                            bit<24> psn_delta,
                            PORT_T final_port) {
        if (final_port == CPU_PORT || final_port >= DROP_PORT) {
            drop();
        } else {
            headers.ethernet.src_addr = src_mac;
            headers.ethernet.dst_addr = dst_mac;
            headers.ipv4.src_addr = src_ip;
            headers.ipv4.dst_addr = dst_ip;
            headers.ipv4.ttl = headers.ipv4.ttl - 1;
            headers.udp.src_port = udp_src_port;
            headers.udp.checksum = 0;
            headers.bth.dest_qp = dst_qp;
            headers.bth.p_key = p_key;
            headers.bth.psn = headers.bth.psn + psn_delta;

            // PRE already selected CPU_PORT. Egress cannot change that port.
            // The existing ICRC controller returns this copy to final_port.
            headers.packet_in.setValid();
            headers.packet_in.egress_port = (bit<16>)final_port;
        }
    }

    table multicast_members {
        key = {
            standard_metadata.egress_rid: exact;
        }
        actions = {
            multicast_member;
            drop;
        }
        size = 32;
        const default_action = drop();
    }

    apply {
        // Ingress initializes multicast_group to zero on every new packet,
        // including CPU reinjection, so repaired copies are never rewritten.
        if (metadata.multicast_group == MULTICAST_GROUP) {
            if (standard_metadata.egress_port != CPU_PORT ||
                standard_metadata.ingress_port == CPU_PORT) {
                drop();
            } else {
                multicast_members.apply();
            }
        }
    }
}

#endif
