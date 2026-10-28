# Measurement results

Raw output of the DPU baseline measurements. `CLAUDE.md` has the tables and what limits each number; this directory has the lines they were computed from.

## 2026-10-25-sweep.txt

The message-size sweep, copied unchanged from the console log of the run (18 lines, 3 runs for each of 6 sizes, 64 B to 64 KB).

- Layout: source and three sinks on the DPU1 ARM cores, relay on the DPU2 ARM cores (see `CLAUDE.md`).
- Programs: `rdma/` as of `origin/main` (7a6bfb5), unmodified, with `sink --verify`.
- Options: `--pool 64 --window 16`, `--msg-size` = `max(size + 64, 4096)`.
- Run on 2026-10-25 around 19:50 (+0800). The lines have no timestamp.

It was written by scratch scripts that `rdma/bench.sh` replaced, so the format differs from what `bench.sh` prints:

```
size=1024 count=300000 pool=64 win=16 | src 1.98 Gbps 0.241 Mmsg/s t=1.243s | relay in 1.98 out 5.93 Gbps t=1.243s cpu=1.24u+0.01s util=101% | sinks 1.98,1.98,1.98 Gbps | verify_ok=3/3
```

- `src`: the source's rate, and messages per second computed from its time `t`.
- `relay in` / `out`: what the relay received and what it sent to the three targets in total (3 times `in`).
- `cpu=`, `util=`: CPU time of the relay process. The relay busy-polls, so this is always one core at about 100% and says nothing about its load.
- `sinks`: the rate each of the three sinks reported.
- `verify_ok=3/3`: how many sinks printed `Verify: 0 corrupted, 0 out of order`.

## What is not here

Some numbers in `CLAUDE.md` were read from the console and not saved as files: the runs without `--verify`, the `--pool` and `--window` combinations, the copies with a smaller `min_rnr_timer` or a header-only `payload_fill`, the `ib_send_bw` reference and the RNR counters. Treat them as less well backed than the sweep above.

Runs from now on can be kept with `rdma/bench.sh -o FILE`, which appends each line with a timestamp and the program directory used.
