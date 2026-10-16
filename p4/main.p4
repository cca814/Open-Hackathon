#include <core.p4>
#include <v1model.p4>

#include "parser.p4"
#include "ingress.p4"

control MyVerifyChecksum(inout headers_t headers, inout metadata_t metadata) {
    apply {
        verify_checksum(
            headers.ipv4.isValid() && headers.ipv4.ihl == 5,
            { headers.ipv4.version, headers.ipv4.ihl,
              headers.ipv4.dscp, headers.ipv4.ecn,
              headers.ipv4.total_len, headers.ipv4.identification,
              headers.ipv4.flags, headers.ipv4.frag_offset,
              headers.ipv4.ttl, headers.ipv4.protocol,
              headers.ipv4.src_addr, headers.ipv4.dst_addr },
            headers.ipv4.hdr_checksum, HashAlgorithm.csum16);
    }
}

control MyEgress(inout headers_t headers,
                 inout metadata_t metadata,
                 inout standard_metadata_t standard_metadata) {
    apply { }
}

control MyComputeChecksum(inout headers_t headers, inout metadata_t metadata) {
    apply {
        update_checksum(
            headers.ipv4.isValid() && headers.ipv4.ihl == 5,
            { headers.ipv4.version, headers.ipv4.ihl,
              headers.ipv4.dscp, headers.ipv4.ecn,
              headers.ipv4.total_len, headers.ipv4.identification,
              headers.ipv4.flags, headers.ipv4.frag_offset,
              headers.ipv4.ttl, headers.ipv4.protocol,
              headers.ipv4.src_addr, headers.ipv4.dst_addr },
            headers.ipv4.hdr_checksum, HashAlgorithm.csum16);
    }
}

control MyDeparser(packet_out packet, in headers_t headers) {
    apply {
        packet.emit(headers.packet_in);
        packet.emit(headers.ethernet);
        packet.emit(headers.ipv4);
        packet.emit(headers.udp);
        packet.emit(headers.bth);
        packet.emit(headers.aeth);
        // BMv2 appends the unparsed payload, padding and trailing ICRC.
    }
}

V1Switch(MyParser(), MyVerifyChecksum(), MyIngress(), MyEgress(),
         MyComputeChecksum(), MyDeparser()) main;
