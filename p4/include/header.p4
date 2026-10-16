#ifndef _HEADER_P4_
#define _HEADER_P4_

typedef bit<48> MAC_ADDR_T;
typedef bit<32> IPV4_ADDR_T;
typedef bit<9> PORT_T;
typedef bit<24> QP_T;

@controller_header("packet_in")
header packet_in_h {
    bit<16> egress_port;
}

@controller_header("packet_out")
header packet_out_h {
    bit<16> egress_port;
}

header ethernet_h {
    MAC_ADDR_T dst_addr;
    MAC_ADDR_T src_addr;
    bit<16> ether_type;
}

header ipv4_h {
    bit<4> version;
    bit<4> ihl;
    bit<6> dscp;
    bit<2> ecn;
    bit<16> total_len;
    bit<16> identification;
    bit<3> flags;
    bit<13> frag_offset;
    bit<8> ttl;
    bit<8> protocol;
    bit<16> hdr_checksum;
    IPV4_ADDR_T src_addr;
    IPV4_ADDR_T dst_addr;
}

header udp_h {
    bit<16> src_port;
    bit<16> dst_port;
    bit<16> len;
    bit<16> checksum;
}

// InfiniBand Base Transport Header
header bth_h {
    bit<8> opcode;
    bit<1> se;  // solicited event
    bit<1> mig_req;  // migration request
    bit<2> pad_count;
    bit<4> transport_header_version;
    bit<16> p_key;
    bit<1> fecn;
    bit<1> becn;
    bit<6> reserved_1;  // fecn/becn/reserved_1 form resv8a, masked in the ICRC
    QP_T dest_qp;
    bit<1> a_req;  // acknowledge request
    bit<7> reserved_2;
    bit<24> psn;
}

// Acknowledge Extended Transport Header, present in RC ACK only.
header aeth_h {
    bit<8> syndrome;
    bit<24> msn;  // message sequence number
}

// RC Send Only: Ethernet / IPv4 / UDP / BTH / payload / padding / ICRC.
// RC ACK:       Ethernet / IPv4 / UDP / BTH / AETH / ICRC.
struct headers_t {
    packet_out_h packet_out;
    packet_in_h packet_in;
    ethernet_h ethernet;
    ipv4_h ipv4;
    udp_h udp;
    bth_h bth;
    aeth_h aeth;
}

// Per-packet scratch space; persistent connection mappings live in ingress.
struct metadata_t {
    bit<1> connection_matched;
    bit<1> icrc_dirty;
}

#endif
