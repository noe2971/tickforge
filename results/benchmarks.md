# Benchmarks

All numbers below were measured by the commands shown; none are estimates.
Every section records the power source and Low Power Mode at the start and
the end of its run (rule in `PROGRESS.md`, "Power source").

## ITCH 5.0 full-day parse throughput — first run, on battery power (2026-11-27)

Kept for comparison. Power was recorded only at the start of this run, and
Low Power Mode was not recorded (it was found switched on later the same day).

Method (locked in `PROGRESS.md`, Rule B, before the first run): stream
`01302019.NASDAQ_ITCH50.gz` with zlib, frame on the 2-byte length prefix, decode every
message into its typed struct and fold the fields into a checksum. 1 warm-up
pass (discarded), then 3 measured passes; the median row is the middle pass by
wall time. MB = 10^6 bytes. "Decode-only" divides by the frame+decode time
alone, excluding time spent inside zlib and file reads.

Command: `tools/run_benchmarks.sh data/01302019.NASDAQ_ITCH50.gz` (run on 2026-11-27, commit `23e7829`)

| Machine | |
|---|---|
| CPU | Apple M5 Pro |
| Cores | 15 logical (5 performance + 10 efficiency) |
| RAM | 24 GiB |
| OS | macOS 26.6.2 (arm64) |
| Power | Battery Power |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| Flags | CMake Release: `-O3 -DNDEBUG` |

zlib 1.2.12; 368366634 messages; 11245883092 uncompressed bytes; 4764426091 compressed bytes; checksum 34e9d033d5d1c3bc

| Pass | Wall s | M msg/s | Uncompressed MB/s | Compressed MB/s | Read+inflate s | Frame+decode s | Decode-only M msg/s | Decode-only MB/s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 18.78 | 19.62 | 598.9 | 253.7 | 15.86 | 2.91 | 126.4 | 3860.1 |
| 2 | 18.78 | 19.62 | 598.9 | 253.7 | 15.86 | 2.91 | 126.4 | 3860.4 |
| 3 | 19.07 | 19.32 | 589.9 | 249.9 | 16.12 | 2.94 | 125.3 | 3824.6 |
| **median** | 18.78 | 19.62 | 598.9 | 253.7 | 15.86 | 2.91 | 126.4 | 3860.1 |

## ITCH 5.0 full-day parse throughput — rerun on mains power

Method (locked in `PROGRESS.md`, Rule B, before the first run): stream
`01302019.NASDAQ_ITCH50.gz` with zlib, frame on the 2-byte length prefix, decode every
message into its typed struct and fold the fields into a checksum. 1 warm-up
pass (discarded), then 3 measured passes; the median row is the middle pass by
wall time. MB = 10^6 bytes. "Decode-only" divides by the frame+decode time
alone, excluding time spent inside zlib and file reads.

Command: `tools/run_benchmarks.sh data/01302019.NASDAQ_ITCH50.gz` (run on 2026-11-28, commit `5de6032-dirty`)

| Machine | |
|---|---|
| CPU | Apple M5 Pro |
| Cores | 15 logical (5 performance + 10 efficiency) |
| RAM | 24 GiB |
| OS | macOS 26.6.2 (arm64) |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| Flags | CMake Release: `-O3 -DNDEBUG` |
| Power at start | AC Power; Low Power Mode off; adapter 30W, battery 48%; charging; (no estimate) |
| Power at end | AC Power; Low Power Mode off; adapter 30W, battery 48%; charging; (no estimate) |

zlib 1.2.12; 368366634 messages; 11245883092 uncompressed bytes; 4764426091 compressed bytes; checksum 34e9d033d5d1c3bc

| Pass | Wall s | M msg/s | Uncompressed MB/s | Compressed MB/s | Read+inflate s | Frame+decode s | Decode-only M msg/s | Decode-only MB/s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 11.95 | 30.84 | 941.4 | 398.8 | 10.09 | 1.85 | 198.9 | 6072.2 |
| 2 | 11.90 | 30.95 | 944.7 | 400.2 | 10.05 | 1.85 | 199.2 | 6082.2 |
| 3 | 11.89 | 30.99 | 946.2 | 400.9 | 10.04 | 1.84 | 199.7 | 6097.7 |
| **median** | 11.90 | 30.95 | 944.7 | 400.2 | 10.05 | 1.85 | 199.2 | 6082.2 |
