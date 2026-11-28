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
