# DPU relay (Baseline)

The relay is an RDMA endpoint. It ends the sender's RC connection, then sends every message it receives to each target over a connection of its own. The NIC does ICRC, ACKs and retransmission on both sides, so the relay never touches a packet header and never computes an ICRC.

```
source ──QP_in──▶ relay ──QP_out[0]──▶ sink 0
                         ├─QP_out[1]──▶ sink 1
                         └─QP_out[2]──▶ sink 2
```

Each message is received into a slot of one registered buffer pool and sent from that same slot to every target, so the payload is not copied. The slot goes back to the receive queue when the last target's send completes.

The programs are separate from `client.c` and `server.c` and do not use their options.

## Build

```sh
cd rdma
make relay sink source     # or: make
```

## Run

Start the sinks, then the relay, then the source. Each program waits for the next one at its TCP exchange. The relay and the source retry a refused connection for 10 seconds.

Three targets on one host, each with its own TCP port (the RDMA device and GID index are the same):

```sh
./sink --device rxe0 --gid-index 1 --port 10011 --count 100 --verify
./sink --device rxe0 --gid-index 1 --port 10012 --count 100 --verify
./sink --device rxe0 --gid-index 1 --port 10013 --count 100 --verify
```

The relay. `--in-*` faces the source and `--out-*` faces the targets; the `--out-*` options default to the `--in-*` ones. `--target` is a sink's TCP address, once per target.

```sh
./relay --in-device rxe0 --in-gid 1 \
        --target 10.0.2.1:10011 --target 10.0.2.1:10012 --target 10.0.2.1:10013 \
        --count 100 --stats
```

The source:

```sh
./source --device rxe0 --gid-index 1 --server-ip <relay-ip> --count 100 --size 1024
```

On the BlueField-3 the two sides are two ports, so pass `--in-device`, `--in-port` and `--in-gid` for the port facing the sender, and `--out-device`, `--out-port` and `--out-gid` for the port facing the targets. The GID index is the RoCE v2 entry with the IPv4 address (`ibv_devinfo -d <dev> -v`).

A successful run ends like this:

```
source: SEND completed: 100 messages, 102400 bytes in <t> s: <rate> Gbps
relay:  Forwarded 100 messages, 102400 bytes in, to 3 target(s) in <t> s: ...
sink:   Received 100 messages, 102400 bytes in <t> s: <rate> Gbps
        Verify: 0 corrupted, 0 out of order
```

`source` fills each message with a sequence number and a pattern. `sink --verify` checks both, so every target must report `0 corrupted, 0 out of order`.

The existing `client` also works as the source for a single message (`./client --device rxe0 --gid-index 1 --server-ip <relay-ip>`); run each sink without `--verify` to see its `Hello RDMA!`.

## Options that matter

| Option | Where | Meaning |
|---|---|---|
| `--msg-size` | relay, sink | Size of one receive buffer, so the largest message. A larger message makes the receive fail. Keep `source --size` at or below it. Default 4096. |
| `--pool` | relay, sink | Receive buffers. When all are in use the sender is held back by RNR retries. Default 16. |
| `--window` | source | Messages in flight. Default 16. |
| `--mtu` | all | Path MTU, 256 to 4096. Default 1024, like `client` and `server`. 4096 needs a link MTU of about 4200. |
| `--count` | all | Messages before exiting. `0` on relay and sink means until Ctrl+C. |

## What a completed send means

The sender's completion comes from the relay's NIC when it has received the message. It does not mean the targets have it. The relay's own sends to the targets are retried by the NICs, and the relay exits with an error if one fails.
