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

Use Linux soft-roce to send and receive the RoCE packets. `rdma/client.c` is the client side which will send the RoCE packet. `rdma/server.c` is the server side which will receive the packet send from client. Since RDMA RC needs to exchange the connection details before start sending the packets, we use TCP connection first to exchange the necessary information, including the `qpn` and `gid`.

### Run

First need to install the dependencies and setup `rxe` device. The `rxe` device will be set in one of the existing network interface card, default is the first one (excluding `lo`).

```
cd rdma
make env    # To specify the nic to embeded rxe, make INTF=<nic-name> env
```

Compile the server code and client code.

```
make client
make server
```

Rum the server code.

```
./server <rxe_device_name> <gid_index>
```

Run the client code.

```
./client <rxe_device_name> <gid_index> <server_ip>
```

> [!NOTE]
> To get the gid of the rxe device, you can run the command `ibv_devinfo -d <rxe_name> -v`. The gid index will be shown in the bottom. Use the one with RoCEv2 and IPv4 address.

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
   22 passed, 0 failed, 0 skipped
   ```

   The server stays up between runs, so the client can be run again directly. Add `-v` to print every packet sent and received.

5. Remove the veth pairs when done:

   ```
   sudo scapy/veth.sh down
   ```
