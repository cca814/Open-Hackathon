# Open-Hackathon

## P4

### Compile

Run from the repository root; the output is `build/main.json`.

```
make -f p4/Makefile build
```

## Controller

`controller/controller.py` runs on the interface wired to the switch CPU port (510). The switch sends it packets whose headers it rewrote, prefixed with a 2-byte `packet_in` header (egress port). The controller recomputes the RoCEv2 ICRC and sends them back with the same 2 bytes as `packet_out`. It needs root (raw sockets) and only the Python standard library.

By default the experiment assumes the RC connections are already established and the switch already holds the rules in `controller/connections.example.txt`, so the controller only repairs ICRCs.

## RDMA

Use Linux Soft-RoCE with either TCP peer discovery (`--server-ip` on the client, no mode flag on the server) or a preconfigured P4 MITM (`--config-file` on both endpoints). In MITM mode, each endpoint connects its RC QP to a virtual P4 GID/QPN and exports its actual, runtime-assigned QPN for the switch rules. Ordinary TCP, ARP, ICMP and other supported traffic crosses the paired switch ports unchanged; RoCE still requires a connection rule.

Follow [P4 MITM mode](#p4-mitm-mode) for the complete sequence, including the required static neighbors, ICRC controller, and rule installation.

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

### P4 MITM mode

Each endpoint connects to a virtual peer that only exists in the switch rules. The switch rewrites the MACs, IPs, UDP source port, destination QPN and PSN of every RoCE packet, and sends it through the controller to repair the ICRC.

```
client node ── <client-nic> ═ <port1-nic> ── port 1   ┐
server node ── <server-nic> ═ <port2-nic> ── port 2   ├ simple_switch (build/main.json)
controller  ── ctl-cpu      ═ sw-cpu      ── port 510 ┘
```

The example configs use these values:

| | Client node | Server node |
|---|---|---|
| Config file | `client.example.conf` | `server.example.conf` |
| Switch port (`switch_port`) | 1 | 2 |
| Real IP (from the GID) / MAC (`local_mac`) | 10.0.0.1 / `02:00:00:00:00:01` | 10.0.0.2 / `02:00:00:00:00:02` |
| Virtual peer IP / MAC / QPN (`remote_*`) | 10.0.0.101 / `02:00:00:00:01:01` / 9001 | 10.0.0.102 / `02:00:00:00:01:02` / 9002 |

#### 1. Check the nodes

On each RDMA node:

- `rdma link show` lists `rxe0` on the interface wired to the switch.
- That interface has the real IP, in the same subnet as the virtual peer (for example `10.0.0.1/24`), so the virtual peer is reached on-link through it.
- `local_mac` in the config file is the MAC of that interface (`ip -br link show <nic>`). The switch writes it as the destination MAC, so a different value makes the NIC drop the packets.
- `switch_port` is the BMv2 port number the node is wired to.

#### 2. Add the static neighbors

Nothing answers ARP for a virtual peer, but rxe needs the peer's MAC from the kernel neighbor table when the QP moves to RTR. Without an entry the endpoint fails with `QP RTR failed: Connection timed out`. Map each node's virtual peer IP to its `remote_mac`:

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

The tables start empty, so nothing crosses the switch until step 5.

#### 4. Start the server, then the client

On the server node, from `rdma/`:

```sh
./server --device rxe0 --gid-index <gid_index> --config-file server.example.conf
```

```
QP ready: local QPN 21, remote QPN 9002
Connection information written to server-connection.json
Receive buffer ready; waiting for the client.
```

On the client node, from `rdma/`:

```sh
./client --device rxe0 --gid-index <gid_index> --config-file client.example.conf
```

```
QP ready: local QPN 21, remote QPN 9001
Connection information written to client-connection.json
Install the P4 rules using both info files, then press Enter to send.
```

The local QPNs are assigned at runtime. Leave the client waiting. `--info-file` changes where the JSON file is written.

#### 5. Install the connection rules

Copy `client-connection.json` and `server-connection.json` to the switch host (for example with `scp`), then, from the repository root:

```sh
python3 controller/render_connections.py client-connection.json server-connection.json > /tmp/mitm-rules.txt
simple_switch_CLI --thrift-port 9090 < /tmp/mitm-rules.txt
```

The CLI prints `Entry has been added` six times: two `forwarding` entries that pair ports 1 and 2, and four `connections` entries for the Send and the ACK in each direction.

#### 6. Send

Press Enter in the client terminal. The client finishes with:

```
WR 2 completed, opcode=0
SEND completed!
```

The server receives the message and exits:

```
WR 1 completed, opcode=128
Received data: Hello RDMA!
```

The controller repairs two ICRCs, the Send to port 2 and its ACK to port 1 (the values differ per run):

```
[controller] roce len=70 egress_port=2 icrc 422f9460 -> 762f8946 (changed) [seen=1 fixed=1]
[controller] roce len=62 egress_port=1 icrc 1ab11003 -> e9581cc0 (changed) [seen=2 fixed=2]
```

#### Run again

The rules carry the real QPNs of this run, so restart `simple_switch` to get empty tables, then repeat steps 4 to 6. The controller, the static neighbors and the CPU veth pair can stay.

#### Clean up

```sh
sudo ip neigh del 10.0.0.101 dev <client-nic>   # client node
sudo ip neigh del 10.0.0.102 dev <server-nic>   # server node
sudo ip link del sw-cpu                          # switch host, after stopping simple_switch
```

#### Troubleshooting

| Symptom | Cause |
|---|---|
| `QP RTR failed: Connection timed out`, and `ip neigh` shows the virtual peer as `INCOMPLETE` | The static neighbor is missing (step 2). |
| After Enter the client prints nothing and spins at 100% CPU; the controller prints nothing | The Send is not crossing the switch: `simple_switch` is not running, the rules are not installed, or `switch_port` does not match the `-i` port numbers. Check `echo "table_dump MyIngress.connections" \| simple_switch_CLI --thrift-port 9090`, then stop the client with Ctrl+C. |
| Same as above, but `tcpdump -ni <port1-nic> udp port 4791` on the switch host shows nothing | `rxe0` is on an interface that is not wired to the switch (`rdma link show`). |

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
