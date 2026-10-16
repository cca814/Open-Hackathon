#ifndef _CONST_P4_
#define _CONST_P4_

// Match the BMv2 --cpu-port setting. 511 is the default BMv2 drop port.
const bit<9> CPU_PORT = 510;
const bit<9> DROP_PORT = 511;

const bit<16> ETHERTYPE_IPV4 = 0x0800;
const bit<8> IP_PROTO_UDP = 17;
const bit<16> UDP_PORT_ROCEV2 = 4791;
const bit<8> BTH_OP_RC_SEND_ONLY = 0x04;
const bit<8> BTH_OP_RC_ACK = 0x11;

const bit<32> CONNECTION_TABLE_SIZE = 4096;

// Indices of MyIngress.stats: where each packet ended up in ingress.
// Read with `counter_read MyIngress.stats <index>` (see stats/README.md).
const bit<32> STAT_DROP_MALFORMED = 0;     // parser/checksum error or not Eth/IPv4/UDP 4791/BTH
const bit<32> STAT_DROP_UNSUPPORTED = 1;   // IPv4 options/fragments, BTH version, opcode
const bit<32> STAT_DROP_BAD_ACK = 2;       // ACK without AETH, with payload or pad
const bit<32> STAT_DROP_BAD_PACKET_OUT = 3;
const bit<32> STAT_PACKET_OUT = 4;         // controller return forwarded to its port
const bit<32> STAT_DROP_INGRESS = 5;       // from the CPU port without packet_out, truncated, TTL <= 1
const bit<32> STAT_DROP_MISS = 6;          // no connection rule, or an explicit drop rule
const bit<32> STAT_DROP_EGRESS = 7;        // rule points at the CPU or drop port
const bit<32> STAT_PUNT_ICRC = 8;          // sent to the controller for ICRC repair
const bit<32> STAT_FAST_PATH = 9;          // translated without touching ICRC fields
const bit<32> STAT_COUNT = 10;

#endif
