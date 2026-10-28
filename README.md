# Open-Hackathon

## P4

### Compile

Run from the repository root; the output is `build/main.json`.

```
make -f p4/Makefile build
```

## Controller

`controller/controller.py` runs on the interface wired to the switch CPU port (510). The switch sends it packets whose headers it rewrote, prefixed with a 2-byte `packet_in` header (egress port). The controller recomputes the RoCEv2 ICRC and sends them back with the same 2 bytes as `packet_out`. It needs root (raw sockets) and only the Python standard library.

By default the experiment assumes the RC QPs are already set up and the switch already holds the rules generated under `config/` (see [config/README.md](config/README.md)), so the controller only repairs ICRCs.

## RDMA

Use Linux Soft-RoCE with either TCP peer discovery (`--server-ip` on the client, no mode flag on the server) or P4 multicast (`--config-file` on every endpoint). In both modes the server sends one `Hello RDMA!` Send and the client receives it.

In P4 multicast mode the server is the sender and each client (`--rid <n>`) is one receiver. Every endpoint connects its RC QP to a virtual P4 GID/QPN and exports its actual, runtime-assigned QPN for the switch rules. The switch replicates each Send to every receiver and returns a single ACK to the sender once all receivers have ACKed. RoCE always needs a rule; ordinary TCP, ARP, ICMP and other supported traffic crosses the switch only if the port pairs in `config/unicast/direct-forward.txt` are loaded.

Follow [P4 multicast mode](#p4-multicast-mode) for the complete sequence, including the required static neighbors, ICRC controller, and rule installation.

### Build

Install the dependencies and create the `rxe0` device on each RDMA node. `make env` attaches `rxe0` to the first network interface (excluding `lo`). If that is not the interface wired to the switch, pass `INTF`; otherwise RDMA traffic bypasses the switch.

```
cd rdma
make env    # or: make INTF=<nic-name> env
```

Compile the server code and client code.

```
make client
make server
```

> [!NOTE]
> Both modes need the GID index of the rxe device. Run `ibv_devinfo -d <rxe_name> -v`; the GID table is at the bottom. Use the RoCE v2 entry with the IPv4 address (`::ffff:<ip>`).

### TCP exchange mode

Start the server, then the client:

```sh
./server --device <rxe_device_name> --gid-index <gid_index>
./client --device <rxe_device_name> --gid-index <gid_index> --server-ip <server_ip>
```

### P4 multicast mode

Every endpoint connects to a virtual peer that only exists in the switch rules. The server sends one RC Send to its virtual peer. The switch replicates it once per receiver, rewrites each copy's MACs, IPs, UDP source port, destination QPN and PSN for that receiver, and sends every copy through the controller to repair the ICRC. Each receiver ACKs its copy. The switch counts the distinct ACKs and forwards only the last one to the server, rewritten as the ACK for the original Send.

```
client node (receivers) ── <client-nic> ═ <port1-nic> ── port 1   ┐
server node (sender)    ── <server-nic> ═ <port2-nic> ── port 2   ├ simple_switch (build/main.json)
controller              ── ctl-cpu      ═ sw-cpu      ── port 510 ┘
```

The example runs two receivers on the client node and uses these values:

|                                            | Receiver rid 1                   | Receiver rid 2    | Sender                           |
| ------------------------------------------ | -------------------------------- | ----------------- | -------------------------------- |
| Node                                       | Client                           | Client            | Server                           |
| Config file                                | `receiver-1.conf`                | `receiver-2.conf` | `server.example.conf`            |
| Info file                                  | `receiver-1.json`                | `receiver-2.json` | `sender.json`                    |
| Switch port (`switch_port`)                | 1                                | 1                 | 2                                |
| Real IP (from the GID) / MAC (`local_mac`) | 10.0.0.1 / `02:00:00:00:00:01`   | same as rid 1     | 10.0.0.2 / `02:00:00:00:00:02`   |
| Virtual peer IP / MAC (`remote_*`)         | 10.0.0.101 / `02:00:00:00:01:01` | same as rid 1     | 10.0.0.102 / `02:00:00:00:01:02` |
| Virtual peer QPN (`remote_qpn`)            | 9101                             | 9102              | 9002                             |
| Initial PSN                                | RQ 5000                          | RQ 6000           | SQ 9000                          |

Receivers on the same node share the port, IP and virtual peer, so their `remote_qpn` must differ: it is the only field that tells their ACKs apart. Receivers can also run on other nodes and ports. Every `receiver-<rid>.json` in the group directory joins the group, up to 32 receivers.

#### 1. Check the nodes

On each RDMA node:

- `rdma link show` lists `rxe0` on the interface wired to the switch.
- That interface has the real IP, in the same subnet as the virtual peer (for example `10.0.0.1/24`), so the virtual peer is reached on-link through it.
- `local_mac` in the config file is the MAC of that interface (`ip -br link show <nic>`). The switch writes it as the destination MAC, so a different value makes the NIC drop the packets.
- `switch_port` is the BMv2 port number the node is wired to.

#### 2. Add the static neighbors

Nothing answers ARP for a virtual peer, but rxe needs the peer's MAC from the kernel neighbor table when the QP moves to RTR. Without an entry the endpoint fails with `QP RTR failed: Connection timed out`. Map each node's virtual peer IP to its `remote_mac` (receivers on the same node share one entry):

```sh
# Client node: remote_gid ::ffff:10.0.0.101, remote_mac 02:00:00:00:01:01
sudo ip neigh replace 10.0.0.101 lladdr 02:00:00:00:01:01 dev <client-nic> nud permanent

# Server node: remote_gid ::ffff:10.0.0.102, remote_mac 02:00:00:00:01:02
sudo ip neigh replace 10.0.0.102 lladdr 02:00:00:00:01:02 dev <server-nic> nud permanent
```

`ip neigh show dev <nic>` must list the entry as `PERMANENT`. The entries are lost on reboot.

#### 3. Start the switch and the controller

On the switch host, from the repository root. `simple_switch` and the controller keep running, so start each in its own terminal.

```sh
make -f p4/Makefile build

# CPU port 510: BMv2 uses sw-cpu, the controller uses ctl-cpu.
sudo ip link add sw-cpu type veth peer name ctl-cpu
for dev in sw-cpu ctl-cpu; do
    sudo sysctl -qw net.ipv6.conf.$dev.disable_ipv6=1
    sudo ip link set dev $dev up
done

sudo simple_switch -i 1@<port1-nic> -i 2@<port2-nic> -i 510@sw-cpu --thrift-port 9090 build/main.json
sudo python3 controller/controller.py -i ctl-cpu -v
```

The tables start empty, so nothing crosses the switch until step 6.

#### 4. Start the receivers

On the client node, from `rdma/`, create one config per receiver from `client.example.conf`, each with its own `remote_qpn` and `rq_psn`:

```sh
sed 's/^remote_qpn .*/remote_qpn 9101/; s/^rq_psn .*/rq_psn 5000/' client.example.conf > receiver-1.conf
sed 's/^remote_qpn .*/remote_qpn 9102/; s/^rq_psn .*/rq_psn 6000/' client.example.conf > receiver-2.conf
```

Start each receiver in its own terminal:

```sh
./client --device rxe0 --gid-index <gid_index> --config-file receiver-1.conf --rid 1
./client --device rxe0 --gid-index <gid_index> --config-file receiver-2.conf --rid 2
```

```
QP ready: local QPN 87, remote QPN 9101
Connection information written to receiver-1.json
Receive buffer ready; waiting for the server.
```

`--rid <n>` writes the info file as `receiver-<n>.json`, the name the rule generator expects. The generator uses `<n>` as that receiver's replication ID (rid) in the switch. Leave the receivers waiting.

#### 5. Start the sender

On the server node, from `rdma/`:

```sh
./server --device rxe0 --gid-index <gid_index> --config-file server.example.conf --info-file sender.json
```

```
QP ready: local QPN 60, remote QPN 9002
Connection information written to sender.json
Install the P4 rules using all info files, then press Enter to send.
```

The local QPNs are assigned at runtime. Leave the server waiting.

#### 6. Load the multicast group

The generator turns every info file in `config/multicast/group1/` into one group, so replace the example files there with this run's files. On the switch host, from the repository root:

```sh
rm config/multicast/group1/*.json   # the example files; git checkout config/multicast/group1 restores them
scp <server-node>:<repo>/rdma/sender.json config/multicast/group1/
scp '<client-node>:<repo>/rdma/receiver-*.json' config/multicast/group1/

mkdir -p config/multicast/output    # the generator does not create it
python3 config/multicast/generate-multicast-group-config.py -g 1
simple_switch_CLI --thrift-port 9090 < config/multicast/output/group1-multicast.txt
```

The generator prints `wrote .../config/multicast/output/group1-multicast.txt`. With two receivers, the CLI prints:

- `node was created with handle` twice, once per receiver.
- `Entry has been added` five times: a copy rule (`MyEgress.multicast_members`) and an ACK rule (`MyIngress.multicast_ack`) per receiver, then the sender rule (`MyIngress.multicast_send`).

[config/README.md](config/README.md#multicast) explains every generated line. Load the file only into a freshly started `simple_switch`, because it assumes the multicast node handles start at 0.

#### 7. Send

Press Enter in the server terminal. The server finishes with:

```
WR 2 completed, opcode=0
SEND completed!
```

Every receiver gets the message and exits:

```
WR 1 completed, opcode=128
Received data: Hello RDMA!
```

The controller repairs one ICRC per receiver copy (to port 1), then one for the ACK that goes to the server (to port 2). The values differ per run:

```
[controller] roce len=70 egress_port=1 icrc 11c936b0 -> 3eedd4be (changed) [seen=1 fixed=1]
[controller] roce len=70 egress_port=1 icrc 11c936b0 -> 0edb6721 (changed) [seen=2 fixed=2]
[controller] roce len=62 egress_port=2 icrc 495f2c9f -> 10b303b3 (changed) [seen=3 fixed=3]
```

The first receiver's ACK stops at the switch. Only the last one reaches the server.

#### Run again

The rules carry the real QPNs of this run, and the generated file assumes fresh multicast node handles. Restart `simple_switch` to get empty tables, then start the receivers and the sender again (steps 4 and 5; the receiver configs can be reused) and repeat steps 6 and 7. The controller, the static neighbors and the CPU veth pair can stay.

#### Clean up

```sh
sudo ip neigh del 10.0.0.101 dev <client-nic>   # client node
sudo ip neigh del 10.0.0.102 dev <server-nic>   # server node
sudo ip link del sw-cpu                          # switch host, after stopping simple_switch
git checkout config/multicast/group1             # switch host, restores the example info files
```

#### Troubleshooting

| Symptom                                                                                                                                   | Cause                                                                                                                                                                                                                                                                                                         |
| ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `QP RTR failed: Connection timed out`, and `ip neigh` shows the virtual peer as `INCOMPLETE`                                              | The static neighbor is missing (step 2).                                                                                                                                                                                                                                                                      |
| `cannot generate group 1: [Errno 2] No such file or directory: '.../output/group1-multicast.txt'`                                         | `config/multicast/output/` does not exist. Create it with `mkdir -p config/multicast/output`.                                                                                                                                                                                                                 |
| `cannot generate group 1: ambiguous ACK identity ...`                                                                                     | Two receivers on the same node use the same `remote_qpn`. Give each receiver config its own value (step 4).                                                                                                                                                                                                   |
| After Enter the server prints nothing and spins at 100% CPU; the controller prints nothing                                                | The Send is not crossing the switch: `simple_switch` is not running, the rules are not installed, or `switch_port` does not match the `-i` port numbers. Check `echo "table_dump MyIngress.connections" \| simple_switch_CLI --thrift-port 9090`, then stop the server with Ctrl+C.                           |
| Same as above, but `tcpdump -ni <port2-nic> udp port 4791` on the switch host shows nothing                                               | `rxe0` is on an interface that is not wired to the switch (`rdma link show`).                                                                                                                                                                                                                                 |
| The receivers print `Received data`, but the server never completes and the controller keeps repairing copies to port 1 about every 70 ms | The switch is waiting for an ACK that never comes. Either `config/multicast/group1/` still holds an example or stale `receiver-<rid>.json`, or an ACK was lost after its receiver had already exited. The server retries forever (`retry_cnt` 7 means no limit in rxe), so stop it with Ctrl+C and run again. |

## Scapy

Use scapy to test the P4 and controller behavior on one machine without real RoCE support. `scapy/client.py` and `scapy/server.py` emulate the two RDMA nodes, and BMv2 runs the P4 program on veth pairs.

```
client.py  ── node-a  ═ sw-p1  ── port 1   ┐
server.py  ── node-b  ═ sw-p2  ── port 2   ├ simple_switch (build/main.json)
controller ── ctl-cpu ═ sw-cpu ── port 510 ┘
```

### Run

Run from the repository root. `simple_switch`, the controller and `server.py` keep running, so start each in its own terminal.

1. Compile the P4 program and create the veth pairs:

   ```
   make -f p4/Makefile build
   sudo scapy/veth.sh up
   ```

2. Start the switch. CPU port 510 is wired to `sw-cpu`, so the controller sends and receives packets on `ctl-cpu`. If the controller uses P4Runtime, start `simple_switch_grpc --cpu-port 510` instead and drop `-i 510@sw-cpu`.

   ```
   sudo simple_switch -i 1@sw-p1 -i 2@sw-p2 -i 510@sw-cpu --thrift-port 9090 build/main.json
   ```

3. Install the connection rules, start the controller, then start the server:

   ```
   simple_switch_CLI --thrift-port 9090 < controller/connections.example.txt
   sudo python3 controller/controller.py -i ctl-cpu -v
   sudo python3 scapy/server.py
   ```

4. Run the client. It prints PASS/FAIL for each check and exits with 0 when all of them pass:

   ```
   sudo python3 scapy/client.py
   ```

   ```
   [PASS] echo #1  (Send PSN 1000 ACKed, echo PSN 5000 ACKed)
   ...
   20 passed, 0 failed, 0 skipped
   ```

   The server stays up between runs, so the client can be run again directly. Add `-v` to print every packet sent and received.

5. Remove the veth pairs when done:

   ```
   sudo scapy/veth.sh down
   ```
