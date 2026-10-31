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

## 2026-10-31-busy.txt

Is the relay's single thread the limit? Written by `rdma/bench.sh --busy -o`, so each line has a timestamp and the program directory. The `relay busy` column is the share of time the relay spent handling completions, from the first message to the end of its loop (see "Is the relay CPU-bound?" in `rdma/RELAY.md`).

- Lines 1 to 3 (14:48): `sink --verify`, one run each of 64 B, 1 KB and 4 KB (200000, 200000 and 100000 messages).
- Lines 4 to 12 (14:52): `--no-verify`, three runs each of the same sizes. These lines end with `verify off`.
- Layout and options as in the sweep above (`--pool 64 --window 16`), except that the programs are the `baseline_latency` copy: `rdma/` with the latency stamp in `payload.h` and `relay --busy`, built on both DPUs on 2026-10-31. `min_rnr_timer` is still 12.
- The network was set up with `dpu-net.sh up` for these runs and put back afterwards. On DPU1 `enp3s0f1s2` (10.0.2.4) came up administratively down the second time; `ip link set enp3s0f1s2 up` fixed it before the `--no-verify` runs. Why `up` did not leave it up is not known.
- During the runs, `ps` on DPU1 showed root's `doca_argus` using about 765% CPU (about 7.6 of the 16 cores) since boot, and another user was logged in. Whether this changes the numbers is not known.
- The 1 KB and 4 KB runs last only 0.2 to 0.8 s. The 1 KB `--no-verify` rate varies from 0.68 to 0.92 Mmsg/s across the three runs.
- `--busy` reads the clock twice per batch of completions, so the rates here may be a little lower than without it. `relay busy` is a lower bound: the `ibv_poll_cq` calls that return completions are not counted.

## 2026-10-31-busy-fast.txt

The same `relay busy` measurement with the test programs' limits taken out: does the relay's thread become the limit once `--verify`, the pattern fill and the long RNR timer are out of the way? 18 lines (3 runs each of 64 B, 256 B, 1 KB, 4 KB, 16 KB and 64 KB), written by `rdma/bench.sh -d baseline_fast_busy --busy --no-verify -o`, so each line has a timestamp.

- Programs: `~/baseline_fast_busy` on both DPUs, a copy of `~/baseline_fast` (`min_rnr_timer = 1`, `payload_fill` writes only the 8-byte sequence number; `qp.c`, `payload.h`, `sink.c` and `source.c` are byte-identical to it) with `relay.c` replaced by the one that has `--busy`. The `relay.c` in `~/baseline_fast` is the one before `--busy`, so nothing else differs. The copy exists only on the DPUs.
- Layout, `--pool 64 --window 16`, network setup and the `doca_argus` load as in `2026-10-31-busy.txt`. No `--verify`, because the pattern is not filled.
- Counts: 300000 messages up to 4 KB, 100000 for 16 KB, 30000 for 64 KB. The runs last 0.4 to 2.2 s. The 4 KB rate varies from 0.23 to 0.40 Mmsg/s across the three runs.
- Compare with the 2026-10-25 table in `CLAUDE.md` (same programs but no `--busy`): 64 B to 1 KB (0.14 to 0.15 Mmsg/s), 16 KB (0.23) and 64 KB (0.059, 92.6 Gbps out) match it. 4 KB does not: 0.23, 0.31 and 0.40 here, 0.41 there. Whether `--busy` or run-to-run spread causes that is not known.

## What is not here

Some numbers in `CLAUDE.md` were read from the console and not saved as files: the runs without `--verify`, the `--pool` and `--window` combinations, the copies with a smaller `min_rnr_timer` or a header-only `payload_fill`, the `ib_send_bw` reference and the RNR counters. Treat them as less well backed than the sweep above.

Runs from now on can be kept with `rdma/bench.sh -o FILE`, which appends each line with a timestamp and the program directory used.
