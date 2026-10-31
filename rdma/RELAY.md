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
| `--pool` | relay, sink | Receive buffers. When all are in use the sender is held back by RNR retries. Default 16. On the BlueField-3 testbed, `--pool 16 --window 16` was about 20 times slower than `--pool 64 --window 16` at 1 KB. |
| `--window` | source | Messages in flight. Default 16. |
| `--latency` | sink | Time each message from the source's post to the sink's receive; see Latency below. |
| `--latency-skip` | sink | Leave the first N messages out of the latency figures. Default 0. |
| `--busy` | relay | At the end, print `Busy: <p>% of <t> s handling completions`: the share of time from the first message to the end that the relay spent working on completions, not spinning on empty polls. See Is the relay CPU-bound below. |
| `--mtu` | all | Path MTU, 256 to 4096. Default 1024, like `client` and `server`. 4096 needs a link MTU of about 4200. |
| `--count` | all | Messages before exiting. `0` on relay and sink means until Ctrl+C. |

## Latency

`source` writes the time it posts each send (`CLOCK_MONOTONIC`, nanoseconds) into bytes 8 to 15 of the message. `sink --latency` subtracts it from the time it polls the receive completion, so the figure is the delay from the source through the relay to that sink. The relay forwards the bytes untouched. The sink prints one line after the throughput line:

```
Latency: n=20000 min=<> p50=<> p90=<> p99=<> p99.9=<> max=<> mean=<> us
```

Read it with these in mind:

- **One clock.** The send time and the receive time must come from the same `CLOCK_MONOTONIC`, so the source and the sink must run on the same machine. On the BlueField-3 testbed they do (all on the DPU1 ARM cores), so the path leaves DPU1 on `p0` and comes back on `p1`: a round trip. If they run on different machines the figure is meaningless; the sink counts a message that arrives before its send time and reports it on a `Latency left out` line.
- **Use `--window 1` for the delay of a single message.** With a larger window, a message waits behind the ones sent before it, and the figure includes that queueing. Both are worth measuring, separately.
- **The times are when the CPU sees the event**, not when the packet is on the wire: the send time is taken just before `ibv_post_send`, and the receive time when the sink polls the completion.
- **The first messages can be slow** while OVS sets up the flow. `--latency-skip N` leaves them out.
- The message must be at least 16 bytes (`--size 16`). The pattern that `--verify` checks starts at byte 16, so the source and sink must be built from the same sources.

```sh
./bench.sh --latency -w 1 -r 1 1024:20000   # adds "latency p50 a,b,c p99 a,b,c us", one value per sink
```

## Is the relay CPU-bound?

The relay is one thread that busy-polls, so `top` shows one core at 100% whether it is working or waiting. `--busy` tells them apart: it adds up the time spent handling completions (receive, the three sends, recycling a slot) and prints it as a share of the time from the first message to the end of the loop.

- **Near 100%**: the loop always has something to do, so the single thread may be the limit.
- **Well below 100%**: the relay waits, so something else limits the rate (RNR retries, the sinks, the link). More threads would not help.

The figure is a lower bound. The `ibv_poll_cq` calls that return completions and the empty polls are not counted, and the span also includes the wait for the last sends. Reading the clock twice per batch costs a little, so compare throughput against a run without `--busy` before trusting the rate from a run with it.

```sh
./bench.sh --busy 1024:300000   # adds "relay busy <p>%"
```

## What a completed send means

The sender's completion comes from the relay's NIC when it has received the message. It does not mean the targets have it. The relay's own sends to the targets are retried by the NICs, and the relay exits with an error if one fails.

Because of this the sender never waits for the relay, so it keeps sending until the relay's pool is empty and then slows down on RNR retries. The throughput you measure is shaped by the RNR timer (`min_rnr_timer` in `qp.c`), not only by the relay.

## On the BlueField-3 testbed

`dpu-net.sh` sets up the SFs, OVS ports and IPs on each DPU, and `bench.sh` runs the message-size sweep (three sinks and the source on DPU1, the relay on DPU2). The layout, the pitfalls found on the hardware (one RDMA process per SF, no GID on `p0`/`p1`, ARP with several IPs on a subnet) and the measured results are in `CLAUDE.md`.

```sh
./dpu-net.sh dpu1 up       # on DPU1; run `dpu2 up` on DPU2
./bench.sh -r 1 1024:20000 # from a machine that can ssh to both DPUs
```
