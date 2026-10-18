#ifndef _ICRC_PARSER_P4_
#define _ICRC_PARSER_P4_

#include "include/const.p4"
#include "include/header.p4"

parser MyParser(packet_in packet,
                  out headers_t headers,
                  inout metadata_t metadata,
                  inout standard_metadata_t standard_metadata) {
    state start {
        transition select(standard_metadata.ingress_port) {
            CPU_PORT: parse_packet_out;
            default: parse_ethernet;
        }
    }

    state parse_packet_out {
        packet.extract(headers.packet_out);
        transition parse_ethernet;
    }

    state parse_ethernet {
        packet.extract(headers.ethernet);
        transition select(headers.ethernet.ether_type) {
            ETHERTYPE_IPV4: parse_ipv4;
            default: accept;
        }
    }

    state parse_ipv4 {
        packet.extract(headers.ipv4);
        transition select(headers.ipv4.ihl, headers.ipv4.frag_offset,
                          headers.ipv4.flags & 3w1, headers.ipv4.protocol) {
            (5, 0, 0, IP_PROTO_UDP): parse_udp;
            default: accept;
        }
    }

    state parse_udp {
        packet.extract(headers.udp);
        transition select(headers.udp.dst_port) {
            UDP_PORT_ROCEV2: parse_bth;
            default: accept;
        }
    }

    state parse_bth {
        packet.extract(headers.bth);
        transition select(headers.bth.opcode) {
            BTH_OP_RC_SEND_ONLY: accept;
            BTH_OP_RC_ACK: parse_aeth;
            default: accept;
        }
    }

    state parse_aeth {
        packet.extract(headers.aeth);
        transition accept;
    }
}

#endif
