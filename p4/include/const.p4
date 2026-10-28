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

// One fixed, single-sender multicast group; member IDs are 0..31.
const bit<16> MULTICAST_GROUP = 1;
const bit<6> MAX_MULTICAST_MEMBERS = 32;

#endif
