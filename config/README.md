# Config

Every `.txt` file here is a `simple_switch_CLI` script for the P4 program in `p4/`: one command per line, no comments. Load one into a running switch with:

```sh
simple_switch_CLI --thrift-port 9090 < <file>.txt
```

### Rule syntax

```
table_add <table> <action> <match key ...> => <action parameter ...>
```

| Part                     | Meaning                                                                                                     |
| ------------------------ | ----------------------------------------------------------------------------------------------------------- |
| `table_add`              | Add one entry to a match-action table.                                                                      |
| `<table>`                | Control block + table name, e.g. `MyIngress.connections`.                                                   |
| `<action>`               | Action to run when the entry matches, e.g. `MyIngress.translate_connection`.                                |
| `<match key ...>`        | One value per table key, in the order of the table's `key = { ... }`. All keys in this program are `exact`. |
| `=>`                     | Separates the match keys from the action parameters.                                                        |
| `<action parameter ...>` | One value per action argument, in the order of the action's signature.                                      |

Value formats:

- Port, QPN, P_Key, PSN: decimal.
- IPv4: dotted, e.g. `10.0.0.1`.
- MAC: 48-bit hex with no colons, e.g. `02:00:00:00:00:02` → `0x020000000002`.
- Opcode: hex, e.g. `0x04` (RC Send Only) or `0x11` (RC ACK).
- PSN delta: 24-bit unsigned, so negative values are written mod 2^24, e.g. −1000 → `16776216`.

### Endpoint fields

The rules are built from each node's endpoint information (the JSON files under `multicast/group<id>/`, also printed by `rdma/` with `--info-file`). A _virtual peer_ is the remote IP/QPN/MAC a node is configured to talk to. It doesn't exist anywhere; the switch only uses it as a match key and rewrites it to the real peer.

| Field         | Meaning                                                       | Used in rules as                        |
| ------------- | ------------------------------------------------------------- | --------------------------------------- |
| `qpn`         | The node's real QPN                                           | `dst_qp` of packets going to this node  |
| `gid`         | The node's real IP, as an IPv4-mapped GID (`::ffff:10.0.0.2`) | Match `src_addr` / rewrite `dst_ip`     |
| `remote_qpn`  | Virtual peer QPN                                              | Match `dest_qp`                         |
| `remote_gid`  | Virtual peer IP                                               | Match `dst_addr` / rewrite `src_ip`     |
| `sq_psn`      | Initial PSN this node sends                                   | Base of `psn_delta`                     |
| `rq_psn`      | Initial PSN this node expects to receive                      | Base of `psn_delta`                     |
| `switch_port` | Switch port the node is plugged into                          | Match `ingress_port` / egress `port`    |
| `local_mac`   | The node's MAC                                                | `dst_mac` of packets going to this node |
| `remote_mac`  | Virtual peer MAC                                              | `src_mac` of packets going to this node |

## unicast

Contain the flow rules which are needed for unicast. Including non-rdma packets forwarding and rdma packet re-write rule.

### Example

Topology used by both files:

| Node | Switch port | Real IP    | MAC                 | QPN | SQ / RQ PSN | Virtual peer IP / MAC / QPN               |
| ---- | ----------: | ---------- | ------------------- | --: | ----------- | ----------------------------------------- |
| A    |           1 | `10.0.0.1` | `02:00:00:00:00:01` | 100 | 1000 / 5000 | `10.0.0.101` / `02:00:00:00:01:01` / 9001 |
| B    |           2 | `10.0.0.2` | `02:00:00:00:00:02` | 200 | 9000 / 2000 | `10.0.0.102` / `02:00:00:00:01:02` / 9002 |

#### `direct-forward.txt`: non-RoCE bridging

```
table_add MyIngress.forwarding MyIngress.forward 1 => 2
```

| Component                            | Value                  | Meaning                                                                                                                                            |
| ------------------------------------ | ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| table                                | `MyIngress.forwarding` | Port-pairing table for ordinary traffic (ARP, TCP, ICMP, non-RoCE UDP). On a miss, the packet is dropped. RoCEv2 (UDP 4791) never uses this table. |
| action                               | `MyIngress.forward`    | Set the egress port.                                                                                                                               |
| key `standard_metadata.ingress_port` | `1`                    | Packet came in on port 1.                                                                                                                          |
| param `port`                         | `2`                    | Send it out of port 2.                                                                                                                             |

Each rule covers one direction, so a port pair needs two lines: `1 => 2` and `2 => 1`.

#### `rdma-translate.txt`: RC connection rewrite

A sends to `10.0.0.101` / QP 9001, and B sends to `10.0.0.102` / QP 9002. The switch rewrites each packet so it reaches the real peer. First rule, for A's Send to B:

```
table_add MyIngress.connections MyIngress.translate_connection 1 10.0.0.1 10.0.0.101 9001 0x04 => 0x020000000102 0x020000000002 10.0.0.102 10.0.0.2 50001 200 65535 1000 0 2
```

Match key, using the values as the packet arrives, before any rewrite:

| #   | Key                              | Value        | Meaning                                                                                               |
| --- | -------------------------------- | ------------ | ----------------------------------------------------------------------------------------------------- |
| 1   | `standard_metadata.ingress_port` | `1`          | Came in on A's port.                                                                                  |
| 2   | `headers.ipv4.src_addr`          | `10.0.0.1`   | Sent by A.                                                                                            |
| 3   | `headers.ipv4.dst_addr`          | `10.0.0.101` | A's virtual peer IP.                                                                                  |
| 4   | `headers.bth.dest_qp`            | `9001`       | A's virtual peer QPN. The RC BTH has no source QP, so this field is what tells two connections apart. |
| 5   | `headers.bth.opcode`             | `0x04`       | RC Send Only. Send and ACK need different PSN deltas, so the opcode is part of the key.               |

Action parameters, i.e. the packet B should see:

| #   | Parameter      | Value            | Meaning                                                                             |
| --- | -------------- | ---------------- | ----------------------------------------------------------------------------------- |
| 1   | `src_mac`      | `0x020000000102` | B's virtual peer MAC.                                                               |
| 2   | `dst_mac`      | `0x020000000002` | B's MAC.                                                                            |
| 3   | `src_ip`       | `10.0.0.102`     | B's virtual peer IP, so to B the packet comes from the peer it was configured with. |
| 4   | `dst_ip`       | `10.0.0.2`       | B's real IP.                                                                        |
| 5   | `udp_src_port` | `50001`          | New UDP source port. The destination port stays 4791.                               |
| 6   | `dst_qp`       | `200`            | B's real QPN.                                                                       |
| 7   | `p_key`        | `65535`          | P_Key `0xffff` (default partition).                                                 |
| 8   | `psn_delta`    | `1000`           | Added to the BTH PSN: B's `rq_psn` − A's `sq_psn` = 2000 − 1000.                    |
| 9   | `msn_delta`    | `0`              | Added to the AETH MSN. It only matters for ACKs; use 0 for fresh QPs.               |
| 10  | `port`         | `2`              | Egress port toward B.                                                               |

The action also decrements TTL and zeroes the UDP checksum. Since the IP, QP and PSN change, the ICRC is no longer valid, so the switch sends the packet through the CPU port first and `controller/controller.py` recomputes the ICRC before it goes out on `port`.

A bidirectional connection needs four rules, one per direction and opcode:

| Line | Key: port, src → dst, QP, opcode         | Packet                 | `psn_delta`                                    |
| ---- | ---------------------------------------- | ---------------------- | ---------------------------------------------- |
| 1    | 1, `10.0.0.1 → 10.0.0.101`, 9001, `0x04` | A Send → B             | B.rq − A.sq = 2000 − 1000 = `1000`             |
| 2    | 2, `10.0.0.2 → 10.0.0.102`, 9002, `0x11` | B ACK → A, acks line 1 | −1000 = `16776216`                             |
| 3    | 2, `10.0.0.2 → 10.0.0.102`, 9002, `0x04` | B Send → A             | A.rq − B.sq = 5000 − 9000 = −4000 = `16773216` |
| 4    | 1, `10.0.0.1 → 10.0.0.101`, 9001, `0x11` | A ACK → B, acks line 3 | `4000`                                         |

An ACK carries the PSN of the Send it acknowledges, so it uses the negative of that Send's delta, not the delta of the Send going in its own direction.

## multicast

Contain the flow rules for multicast configuration. Include the group and the sender, receivers' information in this group.

```python
python3 generate-multicast-group-config.py -g <group_id>
```

The script will read the file under `./group<group_id>`, include one `sender.json` and multiple `receiver-<rid>.json`. Based on the files, generate the flow rule text in `./output/group<group_id>-multicast.txt`.

Load the output only on a freshly started switch, because it assumes BMv2 hands out multicast node handles starting from 0. Only group 1 is supported (`MULTICAST_GROUP` in `p4/include/const.p4`).

### Example

`group1/` describes this group:

| Role            | File              | Switch port | Real IP    | QPN | Virtual peer IP / QPN | Initial PSN |
| --------------- | ----------------- | ----------: | ---------- | --: | --------------------- | ----------- |
| sender          | `sender.json`     |           1 | `10.0.0.1` | 100 | `10.0.0.101` / 9001   | SQ 1000     |
| receiver, rid 1 | `receiver-1.json` |           2 | `10.0.0.2` | 200 | `10.0.0.102` / 9101   | RQ 2000     |
| receiver, rid 2 | `receiver-2.json` |           2 | `10.0.0.2` | 201 | `10.0.0.102` / 9102   | RQ 3000     |
| receiver, rid 3 | `receiver-3.json` |           3 | `10.0.0.3` | 300 | `10.0.0.103` / 9201   | RQ 4000     |

`output/group1-multicast.txt` has four parts, in this order. The sender rule comes last so that no Send is replicated before every copy rule and ACK rule is in place.

#### 1. Replication group

```
mc_mgrp_create 1
mc_node_create 1 510
mc_node_create 2 510
mc_node_create 3 510
mc_node_associate 1 0
mc_node_associate 1 1
mc_node_associate 1 2
```

| Command                        | Arguments             | Meaning                                                                                                                                                                                                                                                                        |
| ------------------------------ | --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `mc_mgrp_create 1`             | group ID              | Create multicast group 1 in BMv2's packet replication engine.                                                                                                                                                                                                                  |
| `mc_node_create <rid> 510`     | rid, port             | Create one replication node per receiver. `rid` is the number in `receiver-<rid>.json`, and each copy carries it as `egress_rid`. Port 510 is the CPU port: every copy goes to the controller first for ICRC repair. The receiver's real port is set in the egress rule below. |
| `mc_node_associate 1 <handle>` | group ID, node handle | Attach each node to group 1. BMv2 returns handles 0, 1, 2, … in creation order, which is why the file only works on a fresh switch.                                                                                                                                            |

#### 2. Per-receiver rewrite (egress)

One rule per receiver. Receiver rid 1:

```
table_add MyEgress.multicast_members MyEgress.multicast_member 1 => 0x020000000102 0x020000000002 10.0.0.102 10.0.0.2 50001 200 65535 1000 2
```

| #   | Component                          | Value            | Meaning                                                                                                                           |
| --- | ---------------------------------- | ---------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| —   | key `standard_metadata.egress_rid` | `1`              | The copy made for `receiver-1.json`.                                                                                              |
| 1   | `src_mac`                          | `0x020000000102` | Receiver's `remote_mac`.                                                                                                          |
| 2   | `dst_mac`                          | `0x020000000002` | Receiver's `local_mac`.                                                                                                           |
| 3   | `src_ip`                           | `10.0.0.102`     | Receiver's `remote_gid`.                                                                                                          |
| 4   | `dst_ip`                           | `10.0.0.2`       | Receiver's `gid`.                                                                                                                 |
| 5   | `udp_src_port`                     | `50001`          | Fixed UDP source port toward receivers.                                                                                           |
| 6   | `dst_qp`                           | `200`            | Receiver's `qpn`.                                                                                                                 |
| 7   | `p_key`                            | `65535`          | P_Key `0xffff`.                                                                                                                   |
| 8   | `psn_delta`                        | `1000`           | Receiver's `rq_psn` − sender's `sq_psn` = 2000 − 1000.                                                                            |
| 9   | `final_port`                       | `2`              | Receiver's `switch_port`. It is written into the `packet_in` header, and the controller sends the repaired copy out of this port. |

Rids 2 and 3 have the same layout, with `dst_qp` 201 / 300, `psn_delta` 2000 / 3000, and receiver 3 using its own IP, MACs and port 3.

#### 3. Per-receiver ACK aggregation (ingress)

One rule per receiver. Receiver rid 1:

```
table_add MyIngress.connections MyIngress.multicast_ack 2 10.0.0.2 10.0.0.102 9101 0x11 => 0 3 1000 0x020000000101 0x020000000001 10.0.0.101 10.0.0.1 50002 100 65535 16776216 1
```

The match key is the receiver's ACK (`0x11`) as it arrives: port 2, `10.0.0.2 → 10.0.0.102`, QP 9101. Receivers 1 and 2 share a port and an IP, so their `remote_qpn` (9101 vs 9102) is the only thing that tells their ACKs apart.

| #   | Parameter      | Value            | Meaning                                                                                                                       |
| --- | -------------- | ---------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| 1   | `member_id`    | `0`              | This receiver's bit in the 32-bit ACK bitmap. Receivers are numbered 0..N−1 in rid order, so this is a position, not the rid. |
| 2   | `member_count` | `3`              | Number of distinct ACKs needed before one aggregate ACK goes to the sender.                                                   |
| 3   | `initial_psn`  | `1000`           | Sender's `sq_psn`, used to compute the aggregate ACK's MSN: PSN − 1000 + 1.                                                   |
| 4   | `src_mac`      | `0x020000000101` | Sender's `remote_mac`.                                                                                                        |
| 5   | `dst_mac`      | `0x020000000001` | Sender's `local_mac`.                                                                                                         |
| 6   | `src_ip`       | `10.0.0.101`     | Sender's `remote_gid`.                                                                                                        |
| 7   | `dst_ip`       | `10.0.0.1`       | Sender's `gid`.                                                                                                               |
| 8   | `udp_src_port` | `50002`          | Fixed UDP source port toward the sender.                                                                                      |
| 9   | `dst_qp`       | `100`            | Sender's `qpn`.                                                                                                               |
| 10  | `p_key`        | `65535`          | P_Key `0xffff`.                                                                                                               |
| 11  | `psn_delta`    | `16776216`       | −1000, the negative of this receiver's egress delta. It maps the ACK's PSN back into the sender's PSN space.                  |
| 12  | `port`         | `1`              | Sender's `switch_port`.                                                                                                       |

The switch drops every ACK except the one that brings the count to `member_count`. That one is rewritten with parameters 4–12 and forwarded to the sender.

#### 4. Reset state and enable the sender

```
register_reset MyIngress.multicast_state
table_add MyIngress.connections MyIngress.multicast_send 1 10.0.0.1 10.0.0.101 9001 0x04 => 3 1000
```

| Component                                  | Value                             | Meaning                                                                                                                                     |
| ------------------------------------------ | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `register_reset MyIngress.multicast_state` | —                                 | Clear the ACK state register: valid bit, current PSN, ACK count and member bitmap.                                                          |
| key                                        | `1 10.0.0.1 10.0.0.101 9001 0x04` | The sender's Send Only to its virtual peer: port 1, `10.0.0.1 → 10.0.0.101`, QP 9001.                                                       |
| `member_count`                             | `3`                               | Must match `member_count` in the ACK rules.                                                                                                 |
| `initial_psn`                              | `1000`                            | First PSN the switch accepts. After that, it only accepts a retransmission of the current PSN, or the next PSN once all members have ACKed. |

A matching Send is sent to group 1 with the ACK-request bit forced on, so every receiver ACKs every message.

`unicast/rdma-translate.txt` and this file both use the key `1 10.0.0.1 10.0.0.101 9001 0x04`, so only one of them can be loaded on the same switch.
