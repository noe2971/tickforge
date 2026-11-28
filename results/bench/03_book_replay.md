## Order book replay — full day, all symbols (Step 2)

Method (locked in `PROGRESS.md`, Rule E, before the first run): books pre-created and pre-sized
from `results/itch_01302019_book_peaks.csv` (measured peaks of the validation run); each pass inflates 1 GiB of whole
frames into memory, then frames, decodes and applies every message in it to the books. "Book s"
is the sum of those framing + decode + book-update segments; inflate is reported separately.
1 warm-up pass (discarded) + 3 measured passes per engine; median by book time. Heap allocations
are counted inside the book segments only.

Command: `tools/run_book_benchmarks.sh data/01302019.NASDAQ_ITCH50.gz results/itch_01302019_book_peaks.csv` (run on 2026-11-28, commit `7f4bfe8`)

| Machine | |
|---|---|
| CPU | Apple M5 Pro |
| Cores | 15 logical (5 performance + 10 efficiency) |
| RAM | 24 GiB |
| OS | macOS 26.6.2 (arm64) |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| Flags | CMake Release: `-O3 -DNDEBUG` |
| Power, fast throughput run (start; end) | start AC Power; Low Power Mode off, adapter 30W, battery 48%; charging; 7:02 remaining; end AC Power; Low Power Mode off, adapter 30W, battery 48%; charging; 6:42 remaining |
| Power, fast latency run (start; end) | start AC Power; Low Power Mode off, adapter 30W, battery 48%; charging; 6:42 remaining; end AC Power; Low Power Mode off, adapter 30W, battery 48%; charging; 6:22 remaining |
| Power, reference throughput run (start; end) | start AC Power; Low Power Mode off, adapter 30W, battery 48%; charging; 6:22 remaining; end AC Power; Low Power Mode off, adapter 30W, battery 49%; charging; 5:12 remaining |
| Power, reference latency run (start; end) | start AC Power; Low Power Mode off, adapter 30W, battery 49%; charging; 5:12 remaining; end AC Power; Low Power Mode off, adapter 30W, battery 51%; charging; 3:59 remaining |

Both engines had identical books at every 1 GiB chunk boundary and at the end of the day (combined hash `6dd14ebb3f3cea3b`).

### Throughput

**Speedup of FastBook over ReferenceBook (median book time): 3.77x.**

FastBook:

Engine `fast`, mode `throughput`: 368366634 messages, 363118215 book messages, 0 book calls returned an error; hash of every book at 11 points of the day (each 1 GiB chunk boundary and the end) `6dd14ebb3f3cea3b`.

| Pass | Book s | M msg/s (all) | M book msg/s | ns per book msg | Inflate s | Wall s | Heap allocations in book segments |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 23.93 | 15.4 | 15.2 | 65.9 | 10.26 | 34.29 | 0 |
| 2 | 24.42 | 15.1 | 14.9 | 67.2 | 10.20 | 34.71 | 0 |
| 3 | 24.29 | 15.2 | 14.9 | 66.9 | 10.37 | 34.75 | 0 |
| **median** | 24.29 | 15.2 | 14.9 | 66.9 | 10.37 | 34.75 | 0 |

ReferenceBook:

Engine `reference`, mode `throughput`: 368366634 messages, 363118215 book messages, 0 book calls returned an error; hash of every book at 11 points of the day (each 1 GiB chunk boundary and the end) `6dd14ebb3f3cea3b`.

| Pass | Book s | M msg/s (all) | M book msg/s | ns per book msg | Inflate s | Wall s | Heap allocations in book segments |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 91.61 | 4.0 | 4.0 | 252.3 | 10.40 | 102.68 | 442865912 |
| 2 | 91.38 | 4.0 | 4.0 | 251.6 | 10.50 | 102.54 | 442865912 |
| 3 | 92.54 | 4.0 | 3.9 | 254.9 | 10.51 | 103.73 | 442865912 |
| **median** | 91.61 | 4.0 | 4.0 | 252.3 | 10.40 | 102.68 | 442865912 |

### Per-call latency (add, cancel, delete, execute, replace)

Every book call timed on its own; values are whole clock ticks converted to ns, so each
percentile is accurate to +/- one tick. The timer's own cost is included, not subtracted.

| Call | FastBook p50 | p99 | p99.9 | ReferenceBook p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| add (A+F) | 41.7 | 250.0 | 416.7 | 166.7 | 750.0 | 1041.7 |
| execute (E+C) | 41.7 | 416.7 | 541.7 | 166.7 | 958.3 | 1291.7 |
| cancel (X) | 0.0 | 250.0 | 416.7 | 41.7 | 458.3 | 583.3 |
| delete (D) | 41.7 | 416.7 | 625.0 | 166.7 | 958.3 | 1250.0 |
| replace (U) | 83.3 | 625.0 | 833.3 | 333.3 | 1125.0 | 1458.3 |

(ns, median over 3 passes.) Every pass, FastBook:

Engine `fast`, mode `latency`: 368366634 messages, 363118215 book messages, 0 book calls returned an error; hash of every book at 11 points of the day (each 1 GiB chunk boundary and the end) `6dd14ebb3f3cea3b`.

Clock: `mach_absolute_time`, 41.667 ns per tick. Timer overhead (10,000,000 back-to-back reads): p50 0.0 ns, p99 41.7 ns, p99.9 41.7 ns, mean 3.85 ns.

| Call | Pass | Count | p50 ns | p99 ns | p99.9 ns | max ns | mean ns |
|---|---|---:|---:|---:|---:|---:|---:|
| add (A+F) | 1 | 164696353 | 41.7 | 250.0 | 416.7 | 78958 | 52.66 |
| add (A+F) | 2 | 164696353 | 41.7 | 250.0 | 416.7 | 271542 | 53.31 |
| add (A+F) | 3 | 164696353 | 41.7 | 250.0 | 416.7 | 78125 | 53.76 |
| **add (A+F)** | **median** | 164696353 | **41.7** | **250.0** | **416.7** | 78958 | 53.31 |
| execute (E+C) | 1 | 8255881 | 41.7 | 375.0 | 541.7 | 21167 | 61.34 |
| execute (E+C) | 2 | 8255881 | 41.7 | 416.7 | 541.7 | 32333 | 62.11 |
| execute (E+C) | 3 | 8255881 | 41.7 | 416.7 | 541.7 | 20958 | 62.69 |
| **execute (E+C)** | **median** | 8255881 | **41.7** | **416.7** | **541.7** | 21167 | 62.11 |
| cancel (X) | 1 | 4669874 | 0.0 | 250.0 | 416.7 | 14917 | 28.87 |
| cancel (X) | 2 | 4669874 | 0.0 | 250.0 | 416.7 | 15833 | 29.30 |
| cancel (X) | 3 | 4669874 | 0.0 | 291.7 | 416.7 | 84458 | 29.65 |
| **cancel (X)** | **median** | 4669874 | **0.0** | **250.0** | **416.7** | 15833 | 29.30 |
| delete (D) | 1 | 158273361 | 41.7 | 416.7 | 625.0 | 37875 | 64.25 |
| delete (D) | 2 | 158273361 | 41.7 | 416.7 | 625.0 | 84208 | 65.24 |
| delete (D) | 3 | 158273361 | 41.7 | 416.7 | 625.0 | 74292 | 65.95 |
| **delete (D)** | **median** | 158273361 | **41.7** | **416.7** | **625.0** | 74292 | 65.24 |
| replace (U) | 1 | 27222746 | 83.3 | 583.3 | 833.3 | 25708 | 128.61 |
| replace (U) | 2 | 27222746 | 83.3 | 625.0 | 833.3 | 59875 | 130.21 |
| replace (U) | 3 | 27222746 | 83.3 | 625.0 | 833.3 | 61375 | 131.55 |
| **replace (U)** | **median** | 27222746 | **83.3** | **625.0** | **833.3** | 59875 | 130.21 |

Every pass, ReferenceBook:

Engine `reference`, mode `latency`: 368366634 messages, 363118215 book messages, 0 book calls returned an error; hash of every book at 11 points of the day (each 1 GiB chunk boundary and the end) `6dd14ebb3f3cea3b`.

Clock: `mach_absolute_time`, 41.667 ns per tick. Timer overhead (10,000,000 back-to-back reads): p50 0.0 ns, p99 41.7 ns, p99.9 41.7 ns, mean 3.74 ns.

| Call | Pass | Count | p50 ns | p99 ns | p99.9 ns | max ns | mean ns |
|---|---|---:|---:|---:|---:|---:|---:|
| add (A+F) | 1 | 164696353 | 166.7 | 750.0 | 1000.0 | 205208 | 228.98 |
| add (A+F) | 2 | 164696353 | 166.7 | 750.0 | 1041.7 | 3135333 | 232.19 |
| add (A+F) | 3 | 164696353 | 166.7 | 750.0 | 1041.7 | 5458917 | 233.14 |
| **add (A+F)** | **median** | 164696353 | **166.7** | **750.0** | **1041.7** | 3135333 | 232.19 |
| execute (E+C) | 1 | 8255881 | 166.7 | 958.3 | 1250.0 | 46042 | 228.44 |
| execute (E+C) | 2 | 8255881 | 166.7 | 958.3 | 1291.7 | 373917 | 233.31 |
| execute (E+C) | 3 | 8255881 | 166.7 | 958.3 | 1291.7 | 485208 | 232.64 |
| **execute (E+C)** | **median** | 8255881 | **166.7** | **958.3** | **1291.7** | 373917 | 232.64 |
| cancel (X) | 1 | 4669874 | 41.7 | 458.3 | 583.3 | 21875 | 65.65 |
| cancel (X) | 2 | 4669874 | 41.7 | 458.3 | 583.3 | 36458 | 67.40 |
| cancel (X) | 3 | 4669874 | 41.7 | 458.3 | 583.3 | 134083 | 68.21 |
| **cancel (X)** | **median** | 4669874 | **41.7** | **458.3** | **583.3** | 36458 | 67.40 |
| delete (D) | 1 | 158273361 | 166.7 | 916.7 | 1208.3 | 5248625 | 233.65 |
| delete (D) | 2 | 158273361 | 166.7 | 958.3 | 1250.0 | 442667 | 239.12 |
| delete (D) | 3 | 158273361 | 166.7 | 958.3 | 1250.0 | 9921708 | 240.30 |
| **delete (D)** | **median** | 158273361 | **166.7** | **958.3** | **1250.0** | 5248625 | 239.12 |
| replace (U) | 1 | 27222746 | 333.3 | 1125.0 | 1416.7 | 410708 | 390.88 |
| replace (U) | 2 | 27222746 | 333.3 | 1125.0 | 1458.3 | 1801458 | 396.37 |
| replace (U) | 3 | 27222746 | 333.3 | 1125.0 | 1458.3 | 408792 | 397.77 |
| **replace (U)** | **median** | 27222746 | **333.3** | **1125.0** | **1458.3** | 410708 | 396.37 |
