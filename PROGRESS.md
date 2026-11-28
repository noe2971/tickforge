# Progress

Session notes, decisions, and open questions for Tickforge.

## 2026-11-25 — Project setup

- Initialized git repo, created public GitHub repo `tickforge` under
  `noe2971`, added MIT `LICENSE` (gh's `--license` flag conflicts with
  `--source`, so the file was written by hand instead of via `gh`).
- Scaffolded CMake project: `include/tickforge/`, `src/`, `tests/`
  (GoogleTest via FetchContent), `bench/` (Google Benchmark via
  FetchContent), `python/` (pybind11 bindings via scikit-build-core, managed
  with uv), `tools/`, `data/` (git-ignored except `README.md`), `results/`
  (git-ignored except `.gitkeep`), `docs/`.
- Added `CMakePresets.json` with three configure/build/test presets:
  `release`, `debug`, `debug-sanitize` (Debug + AddressSanitizer +
  UndefinedBehaviorSanitizer).
- Added `.github/workflows/ci.yml`: builds and tests on Ubuntu and macOS for
  the `debug` and `debug-sanitize` presets, and runs the Python test suite
  via `uv`.
- Added a `commit-msg` git hook (`scripts/hooks/commit-msg`, installed by
  `scripts/install-hooks.sh`) that strips any `Co-Authored-By:` line or line
  containing `Generated with` from commit messages.
- No engine logic yet — this commit is scaffolding only.

## 2026-11-25 — ITCH 5.0 data layer: experiment rules (locked before any data was read)

These rules are committed before the sample day is parsed, so the results
cannot influence them.

### Data source
- File: `01302019.NASDAQ_ITCH50.gz` (Nasdaq TotalView-ITCH 5.0, 30 Jan 2019),
  from `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`, 4,764,426,091 bytes.
- The matching `.md5sum` file is listed in the directory but the server
  returns HTTP 404 for it (checked 2026-11-25). `tools/download_itch.sh`
  therefore falls back to: byte count == server Content-Length, plus
  `gzip -t` (CRC32 + length check of every gzip member). The MD5 it computes
  is recorded here once the download finishes.

### Rule A — choosing the less-liquid Nasdaq stock
Picked mechanically from the full-day message-count report, in this order:
1. Stock Directory (`R`) says: market category `Q`, `G` or `S` (Nasdaq-listed),
   issue classification `C` (common stock), ETP flag not `Y`, authenticity
   `P` (production, not a test symbol), round lot size 100.
2. It has a Nasdaq closing cross (`Q` message, cross type `C`) at a price of
   at least $5.00 (rules out penny stocks).
3. It is not AAPL, MSFT or INTC.
4. Among the stocks left, choose the one whose order-execution count
   (`E` + `C` messages) is closest to 2,000 for the day (about one execution
   every 12 seconds over a 6.5-hour session): active enough to have a real
   book and real fills for later queue-position work, but far less active
   than the large caps. Ties go to the alphabetically first symbol.

### Rule B — parse-throughput benchmark
- Build: `release` preset (CMake Release, Apple clang, `-O3 -DNDEBUG`).
- Input: the full gzip file above, read directly (never unzipped to disk).
- Workload per pass: zlib streaming decompression, framing on the 2-byte
  length prefix, full decode of every message into its typed struct, and a
  handler that folds decoded fields into a checksum (so the compiler can't
  skip the decode).
- Passes: 1 warm-up pass (discarded; warms the OS file cache), then 3
  measured passes. Report every measured pass and the median.
- Metrics, each from wall-clock `std::chrono::steady_clock`:
  messages/s, uncompressed ITCH MB/s, and compressed-file MB/s for the whole
  pass; plus the time split into "decompress + read" and "frame + decode", so
  the parser's own speed is reported separately from zlib's.
  MB = 10^6 bytes.
- Machine details (CPU, cores, RAM, OS, compiler, zlib version) are recorded
  next to the numbers in `results/benchmarks.md`.
- Only measured numbers are reported; nothing is estimated.

## 2026-11-25 — ITCH 5.0 data layer: build log

Done:
- `results/` is tracked; files over 5 MB are kept out by
  `scripts/ignore-large-results.sh` (writes `results/.gitignore`) and a
  `pre-commit` hook. `data/` stays ignored.
- Parser for all 23 ITCH 5.0 types (`include/tickforge/itch/`), streaming
  gzip reader (`src/io/byte_source.cpp`), locate table, symbol filter,
  compact `.tfc` format, `tools/itch_count`, `tools/itch_filter`,
  `bench/itch_throughput`, and one-shot `tools/build_data_layer.sh`.
- 52 C++ tests (pass in release and ASan+UBSan builds) and 12 Python tests for
  `tools/select_less_liquid.py`. A deliberately planted offset bug in the Add
  Order decoder made 3 tests fail, so the tests do detect field errors.
- `tools/download_itch.sh` resume verified: after the download was stopped at
  960,634,880 bytes, re-running it continued from that exact byte.

Checks on a real partial download (validation only, not results):
- The first 900,000,000 compressed bytes held 70,270,574 complete messages
  (up to 10:21:22 ET): 0 unknown types, 0 length mismatches.
- End-to-end dry run on the first 20,000,000 messages: the filter kept
  221,585 messages (AAPL 128,626, MSFT 59,482, INTC 33,473, market-wide 4),
  and re-reading the `.tfc` file with `itch_count` gave the same 221,585.
- Stock locate 1335 (CFG-D) gets a second, identical Stock Directory message
  at 05:16:01, so `R` count = distinct symbols + 1. The locate table
  deduplicates it.

Download (completed 2026-11-27, after the link recovered to ~8-10 MB/s):
- `data/01302019.NASDAQ_ITCH50.gz`, 4,764,426,091 bytes = server
  Content-Length; `gzip -t` passed. MD5 computed locally (Nasdaq's .md5sum is
  unreachable, so not cross-checked): `54b7afd0be7f925f7e9d67a23898827b`.
- The first resumed run gave up after 20 timed-out attempts; the retry loop
  now only stops after 20 attempts in a row with no new bytes.

Process note:
- Commit `f4999e2` was made after a test run that reported 3 failures. They
  came from a stale sanitizer binary still containing the planted bug (the
  source had already been restored); a clean rebuild passed 52/52. The commit
  command had checked ctest's text instead of its exit code. From now on every
  commit goes through `scripts/check.sh`, which rebuilds and fails on ctest's
  exit status (verified: it exits non-zero with the bug planted).
- Commit `f6f49d9` (docs only) was made without running the tests first.

## 2026-11-27 — ITCH 5.0 data layer: full-day results

Full day (`results/itch_01302019_counts.md`, `_by_type.csv`, `_by_symbol.csv`):
- 368,366,634 messages, 11,245,883,092 uncompressed bytes, 8,713 symbols,
  03:03:59.688 to 20:05:00.000 ET. 0 unknown types, 0 wrong lengths.
- Add (44.2%) and Delete (43.0%) are 87% of all messages; executions
  (E + C) are 2.2%. Most orders are cancelled, not filled.
- Check: per-symbol totals sum to 368,366,627; the remaining 7 are the
  locate-0 messages (6 System Event + 1 MWCB Decline Level).

Rule A result (`results/itch_01302019_less_liquid_selection.txt`):
- 1,623 stocks passed filters 1-3. Closest to 2,000 executions: **COLB**
  (Columbia Banking System, Nasdaq Global Select `Q`, common stock), 2,001
  executions, 21,618 adds, 50,739 messages, closing cross $36.49. That is
  about 1/45 of AAPL's 89,735 executions.

Filtered file (`results/itch_01302019_filter_summary.txt`):
- `data/01302019_AAPL_MSFT_INTC_COLB.tfc`, 138,584,914 bytes, 4,538,969
  messages: AAPL 1,668,775, MSFT 1,653,563, INTC 1,165,885, COLB 50,739,
  market-wide 7. Each count equals that symbol's full-day report total, and
  re-reading the `.tfc` with `itch_count` reproduces all four exactly.

Benchmark (`results/benchmarks.md`, Rule B, commit `23e7829`):
- Median of 3 full-day passes: 18.78 s, 19.62 M msg/s, 598.9 MB/s
  uncompressed, 253.7 MB/s compressed. Of that, 15.86 s was zlib inflate +
  file reads and 2.91 s was framing + decoding (126.4 M msg/s on its own).
  The bottleneck is decompression, not the parser.
- Caveat: run on battery power (recorded in the table). Rule B did not
  specify a power source; future runs should use mains power to compare.

## 2026-11-27 — Order book engine (Step 2): experiment rules (locked before any engine code ran on data)

Written and committed before the engines exist, so no result can shape them.

### Power source (applies to every benchmark from now on, including reruns)
- Benchmarks run on mains power. Each run records, at its start and at its
  end, the `pmset -g batt` power source and the macOS Low Power Mode flag
  (`ProcessInfo.isLowPowerModeEnabled`). A run whose power source or Low
  Power Mode changes between start and end is discarded and repeated.
- The Step 1 parse benchmark (Rule B, unchanged otherwise) is rerun under
  these conditions before any Step 2 benchmark. The earlier battery-power
  run stays in `results/benchmarks.md`, labelled as such.
- Checked while writing these rules: the laptop reported `Battery Power`
  and Low Power Mode `on` (it is set for both battery and AC), and the
  monotonic counter (`mach_absolute_time`) ticks every 41.667 ns (24 MHz).

### Rule C — differential fuzz test (fast engine vs reference engine)
- Generator: our own xoshiro256** PRNG and bounded-integer function, so a
  seed gives the same stream with any compiler or standard library.
- Every event is one call made identically on both engines: limit order
  (can cross and fill), market order, full cancel, partial cancel,
  replay-mode add (never matches), replay-mode execute, replay-mode replace.
  Some calls are deliberately invalid (unknown id, dead id, duplicate live
  id, zero shares, more shares than remain, price <= 0), so error paths are
  compared too.
- Prices: mostly within a small band around a random-walking mid (many
  orders share a level, so FIFO order matters), sometimes far away (deep
  book, inserts in the middle of the level array).
- After every event compare: the returned status and values; the exact
  sequence of fills (maker id, taker id, price, shares); the complete book —
  every level on both sides best-first (price, total shares, order count)
  and every order in each level in queue order (id, shares) — and the order
  count. Any difference fails the run, which stops and prints seed + event.
- Volume, release build: 1,000,000 short streams x 100 events (seeds
  20261127 + i) plus 20 long streams x 500,000 events (books of hundreds of
  orders). Sanitizer build (`debug-sanitize`, ASan + UBSan): 100,000 short
  streams x 100 events plus 4 long streams x 250,000 events.
- Pass: zero mismatches and zero sanitizer reports. If a bug is found and
  fixed, the whole run is repeated from the first seed.
- Sensitivity check, done before the real run: plant three bugs in the fast
  engine one at a time — (a) LIFO instead of FIFO inside a level, (b) a
  partial fill that forgets to reduce the level's total shares, (c) an
  emptied level left in the level array. The fuzzer must catch each one. The
  bug is then reverted and everything rebuilt.
- A smaller version runs inside `ctest` so CI exercises it on every push.

### Rule D — replay validation on the real day
Input: `data/01302019.NASDAQ_ITCH50.gz`, all symbols, fast engine, one book
per stock locate. Every check is counted; every exception is reported by
category with examples, never dropped.
1. Executions (`E`, `C`): the order exists in the book of the message's
   locate; executed shares <= remaining shares. For `E` (no price field) the
   order's price must be the best price on its side at that moment. For `C`
   (explicit price) report execution price vs the order's price
   (equal / better for the order / worse), split by the printable flag.
   Also report (information, not pass/fail) how often the executed order was
   at the front of its level's queue.
2. `X`, `D`, `U` refer to a live order; `X` cancels <= remaining shares;
   `A`/`F` and the new reference of `U` are not already live.
3. Crossed book: after every book message during continuous trading — between
   System Event `Q` (start of market hours) and `M` (end of market hours),
   while the stock's latest Stock Trading Action state is `T` (trading) —
   best bid < best ask for that stock. Crossed (bid > ask) and locked
   (bid = ask) are counted separately, with how long each lasted and whether
   a later message with the same timestamp resolved it. The same counts are
   reported outside continuous trading for information.
4. Reconciliation, with a ledger kept independently of the engine: for every
   order, added shares - executed - cancelled must reach exactly zero by its
   last execution, its delete or its replace, or else equal the shares the
   engine still holds for it in the end-of-day book. Per symbol: total added
   = executed + cancelled + removed by delete + removed by replace + shares in
   the end-of-day book.
5. Not applied to the book, but counted and explained: `P` trades (non-cross,
   which on Nasdaq means non-displayed/hidden orders; reference number 0),
   `Q` cross trades, `B` broken trades.
6. Timestamps: count any message whose timestamp is earlier than the previous
   one (the top-N export relies on time order).
7. Real-day differential: replay the whole day through both engines in
   lockstep; after every book message compare status, returned values and the
   best bid/ask (price, shares, orders) of the affected stock; at every whole
   hour and at the end compare the full books of all stocks. Zero mismatches
   required.
- Hypotheses written down before looking (to be confirmed or refuted):
  `C` with printable `N` are executions inside opening/closing/halt crosses
  at the cross price; crossed or locked books, if any, come from halts or
  crosses; the end-of-day book may not be empty because Nasdaq may not send
  deletes for orders still resting at 20:00.
- Output: `results/replay_validation_01302019.md` plus a per-symbol peak-size
  CSV (live orders, levels per side) that the benchmark uses to pre-size.

### Rule E — Step 2 benchmarks (`results/benchmarks.md`)
- Build: `release` preset (Apple clang, `-O3 -DNDEBUG`); machine details and
  power (see above) recorded next to every table.
- Replay throughput: full day, all symbols, both engines, same harness. Each
  pass inflates 1 GiB of whole frames into memory, then times framing +
  decoding + book updates over that buffer, and repeats to the end of the
  file; inflate time is reported separately. Books are created and pre-sized
  before the clock starts from the per-symbol peaks in the Rule D CSV (fast
  engine: pools, order map, level arrays; reference engine: its order hash
  map). 1 warm-up pass (discarded) + 3 measured passes per engine; median by
  book time. Heap allocations during each measured pass are counted and
  reported. Both engines' end-of-day books must hash identically or the run
  is invalid.
- Speedup = reference median book time / fast median book time.
- Latency: same replay, but every book call is timed on its own with
  `mach_absolute_time` (41.667 ns per tick here) into per-type histograms:
  add (`A`+`F`), cancel (`X`), delete (`D`), execute (`E`+`C`), replace (`U`).
  Report p50, p99, p99.9, max and mean per type. Values are whole ticks, so
  each percentile is accurate to +/- one tick; the timer's own overhead
  (back-to-back reads) is measured and reported, not subtracted. 1 warm-up +
  3 measured passes per engine; the median of each statistic over the 3.
- Only measured numbers are written; nothing is estimated.

### Rule F — top-N price-level export
- `top_levels(file, symbol, T, N = 10)` returns the book as it stood just
  before time T: every message for that symbol with timestamp < T is
  applied, none at or after T (same no-future-data rule as the simulator).
- Tested on synthetic streams (including a message exactly at T, which must
  be excluded) and on the real day: for AAPL, MSFT, INTC and COLB, every 30
  minutes from 09:30 to 16:00, the fast and reference engines must return
  identical levels.

## 2026-11-27 — Order book engine (Step 2): build log and results

Built:
- `include/tickforge/book/`: `types.hpp` (shared interface, concept
  `OrderBook`), `object_pool.hpp`, `id_map.hpp`, `fast_book.hpp`,
  `reference_book.hpp`, `itch_replay.hpp` (one book per stock locate),
  `differential.hpp` (fuzz harness), `replay_validator.hpp`, `top_levels.hpp`.
- Tools: `book_fuzz`, `itch_validate`, `itch_engine_diff`, `itch_top_levels`;
  benchmark `bench/book_replay`; scripts `tools/run_book_benchmarks.sh`,
  `tools/bench_env.sh`, `tools/assemble_benchmarks.sh`.
- Design and reasons: `docs/order_book_design.md`.
- Tests: behaviour tests run on both engines (typed tests), pool and hash-map
  tests (the map against `std::unordered_map` on 600,000 random operations),
  a small differential fuzz in `ctest`, replay-validator and top-N tests on
  hand-built ITCH days, and `tickforge_alloc_tests`: global `operator new`
  is replaced by a counter. 1,000,000 mixed operations on a reserved
  FastBook made 0 heap allocations; ReferenceBook allocated on every add.

Rule C, differential fuzz:
- Sensitivity check first. Each planted bug was caught in the first stream
  (seed 20261127): (a) LIFO inside a level at event 1, (b) partial fill not
  reducing the level total at event 8, (c) emptied level left in the array at
  event 13. Reverted, rebuilt from clean, 0 mismatches again.
- Release: 1,000,020 streams, 110,000,000 events, 39,501,407 fills, **0
  mismatches** (19 s on 15 threads; `results/book_fuzz_release.md`).
- Sanitizer (`debug-sanitize`, ASan + UBSan, `halt_on_error=1`): 100,004
  streams, 11,000,000 events, 3,947,921 fills, **0 mismatches, 0 sanitizer
  reports** (79 s; `results/book_fuzz_debug_sanitize.md`).
- All 119 ctest tests pass in release and in the sanitizer build (`scripts/check.sh`).
- Observation: the generator often re-targets ids that have already died,
  so about half of the id-based calls are deliberate "unknown order" cases
  (e.g. 4,218,710 ok executes vs 6,261,501 unknown). Every path still ran
  millions of times.

Rule D, real-day validation (`results/replay_validation_01302019.md`):
- 368,366,634 messages, 363,118,215 book messages: **0 exceptions of any
  kind**. Every E/C hit a live order with enough shares; every E hit an
  order at the best price on its side; no crossed or locked book during
  continuous trading; all 8,695 stocks with book activity reconcile to the
  share; timestamps never go backwards.
- Hypotheses written before the run, now checked:
  - "The end-of-day book may not be empty": **refuted**. Nasdaq deleted
    every order; 0 orders and 0 shares remain at 20:05.
  - "Crossed/locked books, if any, come from halts or crosses":
    **confirmed**. 7 crossed and 6 locked episodes, all outside continuous
    trading, all in stocks paused by LULD (state `P`, reason `LUDP`: DLPN,
    PHUN, SXTC) or quotation-only after a halt (`Q`: EYEN). Orders
    accumulate during a pause until the reopening cross.
  - "C with printable N are cross executions at the cross price": **partly
    confirmed**. 1,621 of the 6,582 non-printable C are within 1 s of that
    stock's opening (442), closing (1,112) or halt (67) cross. 1,620 of
    those trade at the order's price or better, as an auction should. The
    other 4,961 are not near any cross; 4,960 of them traded at a *more
    aggressive* price than the order's displayed price (mostly one cent).
    Why these are non-printable is not established by these data.
- Not in the hypotheses, explained or reported:
  - All 152,304 printable C traded at a price more aggressive than the
    order's displayed price: 152,056 by exactly one cent, 248 by less. This
    is consistent with orders displayed one tick away from a price that
    would lock the market (Reg NMS forbids displaying a locking quote) but
    executable at their real limit. That is a hypothesis matching the
    numbers, not proven from the feed.
  - 7,056 of 8,096,995 E executions (0.087%) hit an order that was not first
    in its price level's queue in our rebuilt book. All 7,056 executed orders
    were plain A/F adds (none created by a replace), 7,032 were during market
    hours, and they cluster at 09:xx (3,672) and 14:xx (2,122). (14:00 ET
    that day was the FOMC statement; that is outside knowledge, not from the
    data.) Cause not established. It matters for Step 3: strict FIFO by
    arrival holds for 99.91% of executions, not all.
  - `P` trades (hidden, non-displayed orders): 1,326,184 trades, 173,244,448
    shares, all with order reference 0, so they cannot touch the book. `Q`
    crosses: 8,713 opening, 8,713 closing, 4 halt/IPO. `B` broken: 116.
- Book sizes: peak 1,742,866 live orders across the market; largest book
  AAPL 42,774 orders; most levels on one side 3,143 (AMZN bids). AAPL, MSFT
  and INTC had orders from $0.0001 to $199,999.99, which is why the design
  rejects a price-indexed array (see the design doc).
- Real-day lockstep diff (`results/engine_diff_01302019.md`): all
  363,118,215 book messages gave identical results and best bid/ask in both
  engines; 17 full-book comparisons (whole hours + end) covered 11,633,120
  orders; **0 mismatches**.
- Process note: the post-hoc breakdowns (report section 6) were added after
  the first run, to explain what it found. They are descriptive and change
  no rule; the report was regenerated with them, and the 0-exception result
  was the same.

Rule F, top-10 export (`results/top_levels_check_01302019.txt`): AAPL, MSFT,
INTC, COLB at 14 times from 09:30 to 16:00: **56/56 identical** between the
engines. The same export read from the full `.gz` matches the `.tfc` one
(COLB, 12:00).

Rule E amendment (made before any Step 2 benchmark ran): the validation run
showed that every book is empty at the end of the day, so "end-of-day books
hash identically" would be trivially true. The benchmark instead hashes every
book at each 1 GiB chunk boundary and at the end (outside the timed segments),
and both engines must agree on that combined hash.

Process note: while planting the Rule C bugs, one run reused a stale binary.
macOS ships GNU Make 3.81, which compares file times to the whole second, so
a header edited in the same second as the previous build was not recompiled.
Fixed by forcing recompiles for the planted-bug runs; `scripts/check.sh` and
the benchmark script now always build with `--clean-first` (about 10 s).

Benchmarks: not run yet. The laptop is still on battery power with Low Power
Mode on (checked at 20:09 UTC); the scripts refuse to run off mains.

## 2026-11-28 — Step 1 parse benchmark rerun on mains power

- Power: `AC Power`, Low Power Mode off, at the start and end of the run (30 W
  adapter; battery 48%, charging). Low Power Mode had been set to on for the
  power adapter; it was switched off in System Settings before the run.
- Rule B unchanged. Median of 3 passes: **11.90 s**, 30.95 M msg/s, 944.7 MB/s
  uncompressed, 400.2 MB/s compressed; read+inflate 10.05 s, frame+decode 1.85 s
  (199.2 M msg/s decode-only). Same checksum `34e9d033d5d1c3bc` as the
  battery run, so the same work was done.
- The battery run (18.78 s median) stays in `results/benchmarks.md` as its
  own section. Its Low Power Mode state was not recorded.
- The run is labelled `5de6032-dirty`: the only uncommitted changes were to
  the benchmark scripts (recording adapter wattage and battery state), not
  to any code that was measured.
- The scripts now record, at the start and end of every run, adapter wattage
  and battery state as well as the power source and Low Power Mode.

## 2026-11-28 — Step 2 benchmarks (Rule E), on mains power

`results/bench/03_book_replay.md` (in `results/benchmarks.md`), commit
`7f4bfe8`, all four runs on `AC Power` with Low Power Mode off at start and
end (30 W adapter; battery 48% -> 51%, charging). Medians of 3 full-day
passes after 1 warm-up, all 8,695 active stocks, books pre-sized from the
validation peaks.

| | FastBook | ReferenceBook |
|---|---:|---:|
| Book time (frame + decode + book update) | 24.29 s | 91.61 s |
| Book messages per second | 14.9 M | 4.0 M |
| ns per book message | 66.9 | 252.3 |
| Heap allocations inside the book segments | **0** | 442,865,912 |
| Inflate (zlib + file read), reported separately | 10.37 s | 10.40 s |

- **Speedup 3.77x** (91.61 / 24.29). Both engines had identical books at all
  11 checkpoints (every 1 GiB chunk boundary and the end), combined hash
  `6dd14ebb3f3cea3b`; 0 book calls returned an error.
- Latency, ns, median of 3 passes (clock tick 41.667 ns; +/- one tick; the
  timer's own cost, mean 3.85 ns per pair of reads, is included):

| Call | Count | Fast p50 | p99 | p99.9 | Ref p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|---:|
| add (A+F) | 164,696,353 | 41.7 | 250.0 | 416.7 | 166.7 | 750.0 | 1,041.7 |
| execute (E+C) | 8,255,881 | 41.7 | 416.7 | 541.7 | 166.7 | 958.3 | 1,291.7 |
| cancel (X) | 4,669,874 | 0.0 | 250.0 | 416.7 | 41.7 | 458.3 | 583.3 |
| delete (D) | 158,273,361 | 41.7 | 416.7 | 625.0 | 166.7 | 958.3 | 1,250.0 |
| replace (U) | 27,222,746 | 83.3 | 625.0 | 833.3 | 333.3 | 1,125.0 | 1,458.3 |

- "0.0 ns" means the call finished within one clock tick, i.e. under 41.7 ns.
  FastBook's mean cancel was 29.30 ns, measured by the same timer.
- Largest single calls (max over a pass) reached tens to hundreds of
  microseconds for FastBook (add max 78,125-271,542 ns across the 3 passes) and milliseconds for
  ReferenceBook (delete up to 9,921,708 ns). The cause is not measured.
  Candidates are OS interrupts and first-touch page faults in pre-reserved
  pool memory, which counts as 0 heap allocations but still faults.
- Timing every call costs about 2 s on the day: FastBook's book time was 26.41 s (median; `results/bench/raw/run_log.txt`)
  in latency mode vs 24.3 s in throughput mode.
- Not measured yet: a profile of where FastBook's 66.9 ns per message goes.
  Cache misses on the id map, order and level are the obvious guess, but it
  is only a guess until measured.

## 2026-11-28 — Simulator (Step 3): queue-jump investigation and experiment rules (locked before the simulator ran on any data)

### Investigation: the 7,056 `E` that hit an order not first in its queue

Done first, as asked, before any queue model existed. Tool
`tools/itch_queue_jumps` (`include/tickforge/book/queue_jumps.hpp`, tests in
`tests/test_queue_jumps.cpp`); full-day report `results/queue_jumps_01302019.md`.
It is analysis code that looks ahead in the feed and is not used by the
simulator. It found the same 8,096,995 `E` and 7,056 jumps as Step 2.

- **Every one of the 8,096,995 `E` executions hit the order with the smallest
  order reference number at its price level: 0 exceptions**, the 7,056 jumps
  included.
- Every jumping order was a *late add*: a reference number smaller than one the
  same stock had already published (measured per stock; across stocks the
  numbers are not in time order). The day has 813,636 late adds: 813,633
  between 09:30:00.000565 and 10:00, 167,367 of them within 1 s of the
  market-open event (09:30:00.000036), and 3 at 16:00:00. 531,230 appeared
  more than an hour after their stock had published a larger reference; 8
  within 1 ms. None of the 59,379 skipped orders was a late add.
- 3,420 jumps: every skipped order also executed at the same nanosecond
  timestamp. 3,636 jumps: a skipped order kept resting (25,747 orders), and
  later 24,456 of those executed by `E`, 1,290 were deleted, 1 partly
  cancelled, none was left at the end. 0 jumps shared a timestamp with a `Q`
  cross. The 14:00 cluster is 09:30 late adds still resting when the price
  reached them.
- Reading (fits every number above; the feed does not state it): orders
  entered before the open that only become active at 09:30 are published
  then, but keep the time priority of their entry. Nasdaq ranks a price level
  by entry time, and the order reference number records entry order. Step 2's
  book ranked by publication time, which differs only for these orders.
- **Decision: queue priority inside a price level is reference-number order.**
  No class of execution needs special treatment. Any execution that still
  contradicts the queue model in the simulator is counted (Rule H, anomalies)
  and gives no fill.

### Rule G — shadow orders and their place in the queue
- One stock per run, from the replayed file (`.tfc` for development; `.gz`
  only where a test needs the full day). The real book is rebuilt from the
  feed exactly as in Step 2. Shadow orders never change it.
- Every real order's priority key is its reference number. A shadow order gets
  key K = the largest reference number that stock has published when the order
  arrives at the exchange. It is behind every real order with reference ≤ K
  and ahead of every one with reference > K. A late add (reference ≤ K
  published after the shadow arrived) therefore goes *ahead* of it.
  Shadow orders at one price rank among themselves by arrival.
- `ahead` = shares of real orders at the shadow's side and price with
  reference ≤ K. At arrival it is the level's whole size. It falls when such
  an order is executed, cancelled, deleted or replaced (a replaced order comes
  back with a new, larger reference, so behind), and rises when a late add
  joins.
- Partial cancel of a shadow order keeps its key. Replace = cancel + new order
  with a new key (it loses priority, like ITCH `U`). The new order's size is
  the size given, and a replace arriving after the order fully filled is
  rejected (too late).
- Rejections at arrival: shares 0 or price ≤ 0; post-only that would cross the
  real book; any order that would cross the strategy's own resting shadow
  order on the other side (self-cross). Shadow orders never trade with each
  other.

### Rule H — fills
Shadow orders fill only while the stock's latest Stock Trading Action state is
`T`, or no `H` message has been seen. Fills are at the shadow's own price
(it is the resting order), except taking, which fills at the real price.
- Taking at arrival (both models): a shadow order whose limit crosses the real
  opposite best takes displayed liquidity best price first, at each real
  order's price, up to that order's shares minus what shadow orders already
  took from it. Taken shares stay used until the real order's remaining size
  falls below them. Taker fee. An IOC remainder is cancelled; otherwise it
  rests. A shadow order takes liquidity only at its arrival.
- **Queue-aware** resting fills, best-priced shadow first, then by key:
  1. Real execution (`E`, or `C` at its execution price e) of x shares of
     real order R on the shadow's side. A shadow buy at P fills if P ≥ e
     (sell: P ≤ e), ahead = 0, and R is not ahead of it. The shadows together
     take at most x shares. If R is ahead of it, ahead falls instead. An
     execution behind a shadow, or at a worse price, while ahead > 0 is
     counted as an anomaly and gives no fill.
  2. A real add or replace on the other side at a price crossing a resting
     shadow (ahead = 0) fills it, up to that order's shares: in reality the
     newcomer would have traded with it. If ahead > 0, anomaly and no fill.
  3. Hidden-order trades (`P`) and crosses (`Q`) never fill queue-aware
     shadows: the feed doesn't say whether they could have reached a
     displayed order.
- **Naive "filled on touch"**: the whole remaining size fills at the shadow's
  price at the first real print at or through its price (`E`, `C`, `P` or
  `Q`; either side) or the first real order on the other side priced to cross
  it. Every queue-aware trigger is also a naive trigger, so for orders that
  do not depend on fills, naive fills at least as much and no later.
- Assumption (both models): no market impact. A real order behind a shadow
  still trades as in the feed after the shadow filled.

### Rule I — latency, event order and no look-ahead
- Two configurable delays: order entry (strategy to exchange) for place,
  cancel and replace; market data (exchange to strategy) for every real
  message, fill and order acknowledgement.
- A decision made at strategy time τ takes effect at the exchange at
  τ + order latency, after every real message timestamped at or before that
  time. Real messages win ties, so a shadow joins behind anything that
  arrived in the meantime.
- Event order at equal times: real message, shadow action at the exchange,
  measurement (markout, P&L sample), delivery to the strategy, strategy timer.
- The strategy sees only its own copy of the book, updated from delivered
  messages, its own delivered fills and acknowledgements, and its clock. It
  never sees the exchange-side book, queue positions or any message
  timestamped after τ − market-data latency. The simulator processes messages
  one at a time and never reads ahead for the strategy.
- At the run's end time the simulator cancels every shadow order (no
  latency), calls the strategy's end hook, and fixes the final P&L. After
  that, messages are read only to finish markouts.

### Rule J — accounting
- Cash and fees in integer micro-dollars; prices stay in ITCH's $0.0001 units.
- Fees per share, configurable: maker rebate (default $0.0020) and taker fee
  (default $0.0030). These are illustrative defaults in the shape of Nasdaq's
  schedule, not a quote of a 2019 tier.
- Mark-to-market P&L = cash + inventory × real midpoint. Sampled every second
  from start to end, at every fill, and at the end.
- Markouts at 100 ms, 1 s and 10 s per fill: side sign × (real mid after all
  messages timestamped ≤ fill time + horizon − real mid just before the
  fill's trigger), in dollars per share (side sign +1 for buys). Negative
  means the price moved against the fill (adverse selection). Also reported:
  side sign × (that later mid − fill price). A markout with a one-sided book
  at either end, or a horizon past the last message, is counted as
  unavailable.

### Rule K — tests (all on hand-built streams unless named)
- Queue: an order behind 500 shares fills only after 500 shares trade.
  Cancels and deletes ahead move it up. A late add goes ahead. Partial fills.
  A replace loses priority; a partial cancel keeps it.
- Latency: a shadow order arriving 5 µs after a real order at its price is
  behind it, and the same order with zero latency is ahead of it. Cancels
  arriving after a fill are rejected as too late.
- Taking, depletion, IOC, post-only, self-cross, fees and rebates, no fills
  while halted, markouts and MTM checked to the micro-dollar.
- Look-ahead: alter, drop and add messages after a time T (hand-built and on
  the COLB part of the `.tfc`). Every strategy decision made at or before
  T + market-data latency must be identical. Every callback asserts that the
  strategy has seen no message timestamped after τ − market-data latency.
- Naive ≥ queue-aware: for a fixed schedule of orders (random hand-built
  streams, and COLB/MSFT in the `.tfc`), every order's naive filled shares are
  ≥ its queue-aware filled shares, and its naive first fill is no later.
- Python: a strategy written in Python runs through pybind11. A Python copy of
  the C++ demo strategy gives identical fills on COLB.
- Tests that need files in `data/` skip with a message when the file is
  missing.

### Rule L — demonstration run (`results/sim_demo_01302019.md`)
- Data: `data/01302019_AAPL_MSFT_INTC_COLB.tfc`; stocks COLB (picked in Step 1
  for queue work) and MSFT (long queues).
- Strategy `JoinBestQuoter`, fixed before running: from 09:35:00 to 15:55:00
  ET, keep one 100-share post-only buy at the best bid and one 100-share
  post-only sell at the best ask, as the strategy sees them. When that side's
  best price changes, replace the order at the new best. No buying at
  inventory ≥ +500, no selling at ≤ −500. No flattening: P&L is marked at the
  midpoint at 15:55:00.
- Grid: fill model {queue-aware, naive} × order latency {1 µs, 100 µs, 1 ms};
  market-data latency 0; default fees.
- Reported per run, measured only: orders placed / rejected, fills, filled
  shares, filled shares / placed shares, inventory range, cash, fees and
  rebates, P&L at 15:55, markouts (share-weighted mean and count available)
  at each horizon.
- Hypotheses written before running: (1) naive fills more shares than
  queue-aware in every cell; (2) queue-aware filled shares fall as latency
  rises; (3) queue-aware fills have worse (more negative) 1 s and 10 s
  midpoint markouts than naive fills.
- No timing measurement is part of Step 3. If one is added, the mains-power
  rule applies.

## 2026-11-28 — Simulator (Step 3): build log and results

Built (commit `e3ecaf5`; design note `docs/simulator_design.md`):
- `include/tickforge/sim/`: `types.hpp`, `strategy.hpp` (C++ `Strategy` and
  `Context`), `simulator.hpp` (`Simulator` ITCH handler, `run_file`),
  `strategies.hpp` (`JoinBestQuoter`, `ScheduledOrders`); `src/sim/`.
- Python: `python/src/bindings.cpp` (subclassable `tickforge.Strategy`,
  `tickforge.simulate`), `python/tickforge/strategies.py` (`JoinBestQuoterPy`).
  `python/uv.lock` is now tracked (`uv run` created it).
- Tools: `itch_queue_jumps` (investigation), `sim_demo` (Rule L).
- 149 C++ tests pass in release and in ASan + UBSan (`scripts/check.sh`); 17
  Python tests pass. The tests that need `data/` skip with a message when the
  file is missing.

Rule K checks, all passing:
- Hand-built: behind 500 shares, it fills only after 500 trade (then 60 + 40
  in two partial fills, cash checked to the micro-dollar). Cancels and
  deletes ahead move it up, and a cancel behind it doesn't. A late add goes
  ahead. Partial cancel keeps priority; replace loses it. With 10 µs latency
  the order queues behind a real order that arrived 5 µs before it; with 0
  latency it is ahead and fills at 2 ms instead of 4 ms; arriving at the
  same nanosecond puts it behind. A cancel arriving after the fill is
  rejected as too late. Also: taking without reusing liquidity, IOC,
  post-only and self-cross rejections, no fills while halted, crossing adds,
  markouts at all three horizons and MTM to the micro-dollar.
- Look-ahead, random streams: 12 streams × 4 latency settings = 48 pairs of
  runs whose futures differ after T. Decisions up to T + market-data latency
  were identical in all 48, and later decisions differed in all 48. COLB,
  with the afternoon altered after 12:00 (every third message dropped, adds
  moved 5 cents and doubled): identical decisions up to 12:00:00.00005, and
  the afternoon differs. 0 look-ahead violations in every run.
- Naive ≥ queue-aware, per order (fill-independent strategies): held for
  every order of 150 random streams (naive 21,955,439 shares, queue-aware
  586,357), of COLB (4,558 orders; 23,700 vs 13,375) and of MSFT (4,558
  orders; 236,900 vs 203,416).
- The Python copy of `JoinBestQuoter` gives the same decisions, fills and P&L
  as the C++ one on COLB.

Rule L demonstration (`results/sim_demo_01302019.md`, COLB and MSFT, 09:35 to
15:55, every run 0 queue anomalies, 0 look-ahead violations):

| Stock | Model | Latency | Fills | Filled shares | P&L at 15:55 | 10 s mid move (¢/sh) | 10 s vs fill price (¢/sh) |
|---|---|---|---:|---:|---:|---:|---:|
| COLB | queue-aware | 1 µs | 338 | 23,436 | $32.35 | -1.457 | -0.397 |
| COLB | queue-aware | 100 µs | 468 | 35,040 | $87.34 | -0.949 | -0.415 |
| COLB | queue-aware | 1 ms | 449 | 33,130 | $82.04 | -1.021 | -0.500 |
| COLB | naive | 1 µs | 887 | 88,700 | $119.90 | -1.383 | -0.225 |
| COLB | naive | 100 µs | 719 | 71,900 | $137.30 | -1.220 | -0.131 |
| COLB | naive | 1 ms | 545 | 54,500 | $166.50 | -1.233 | -0.061 |
| MSFT | queue-aware | 1 µs | 14,572 | 955,999 | -$1,296.88 | -0.804 | -0.295 |
| MSFT | queue-aware | 100 µs | 14,810 | 1,069,875 | -$1,997.77 | -0.691 | -0.359 |
| MSFT | queue-aware | 1 ms | 16,767 | 1,238,270 | -$2,274.48 | -0.633 | -0.391 |
| MSFT | naive | 1 µs | 21,672 | 2,167,200 | -$647.60 | -0.730 | -0.192 |
| MSFT | naive | 100 µs | 20,198 | 2,019,800 | -$435.40 | -0.697 | -0.193 |
| MSFT | naive | 1 ms | 17,390 | 1,739,000 | -$478.00 | -0.686 | -0.207 |

Hypotheses (written before the run):
1. "Naive fills more shares than queue-aware in every cell": **confirmed** (6
   of 6; 1.4× to 3.8× as many shares).
2. "Queue-aware filled shares fall as latency rises": **refuted**. MSFT rose
   (955,999 → 1,069,875 → 1,238,270); COLB rose from 1 µs to 100 µs and dipped
   slightly at 1 ms. A post-hoc breakdown (added after the first run,
   descriptive only, in the report) shows two opposite effects. Fills from
   real executions did fall with latency (MSFT 14,331 → 10,612 → 9,701). Fills
   from a real order arriving on the other side at our price rose (MSFT 223 →
   4,172 → 7,031; COLB 1 → 151 → 130). Reading, not proven: a slow strategy
   leaves stale quotes in place after the market moves, and incoming orders
   pick them off. The half-spread captured per filled share fell with latency
   (MSFT queue-aware 0.509¢ → 0.332¢ → 0.242¢), which fits that reading.
3. "Queue-aware fills have worse 1 s and 10 s midpoint markouts than naive":
   **confirmed only at 1 µs**. At 100 µs and 1 ms they were less negative than
   naive for both stocks. Measured against the fill price, queue-aware was
   worse than naive in all 6 cells at all three horizons. Much of the adverse
   move happens *before* a queue-aware fill (between the quote going stale and
   the fill), so the midpoint just before the fill already includes it.
- Inventory reached ±599 under queue-aware fills: the rule stops buying at
  ≥ +500, so a 100-share buy may still fill from +499. Naive fills whole
  100-share orders, so it stops at exactly ±500.
- P&L: on MSFT the strategy lost money under every setting, and the
  queue-aware loss was 2.0× to 4.8× the naive one. Naive's P&L is higher in
  all 6 cells. Every P&L above includes the illustrative maker rebate; with
  no rebate, every cell would be lower by its rebates column in the report.

## 2026-11-28 — CI: red since `5de6032`, cause and fix

- Every CI run from `5de6032` onwards (runs #9 to #12, and the three Step 3
  pushes) failed in exactly one of the six jobs: `cpp (ubuntu-latest,
  debug-sanitize)`. The macOS jobs, the Ubuntu debug job and both Python jobs
  passed every time.
- Cause (from `gh run view --log-failed`): the three `HotPathAllocations`
  tests aborted with AddressSanitizer `alloc-dealloc-mismatch (operator new vs
  free)`. `tests/test_book_alloc.cpp` replaced only the plain and array forms
  of `operator new` (with `malloc`) and `operator delete` (with `free`). GCC
  15's `std::stable_sort`, which GoogleTest runs before the tests, takes a
  temporary buffer from the *nothrow* `operator new`. Under ASan on Linux that
  form was still ASan's own, and our `free()`-based `operator delete` released
  it. ASan checks for this mismatch by default on Linux, not on macOS, which
  is why the local gate missed it.
- The three causes suspected for the failure were checked and were not it.
  (1) Before Step 3 no test read `data/`. The two new `.tfc` tests already
  skipped in CI on both OSes. (2) The Step 2 fuzz tests take 2 to 3 s in
  CI; the long `book_fuzz` run is not part of `ctest`. (3) GCC 15.2.0 built
  all our code with no warning or error under `-Wall -Wextra -Wpedantic`.
- Fixes:
  - `test_book_alloc.cpp` now replaces every form: plain, array, nothrow,
    aligned and sized. Each allocates through one counted path and frees
    with `free()`. A new regression test calls all 12 allocation forms
    directly and requires each to be counted and correctly aligned; another
    runs the `std::stable_sort` path. Against the old code the first test
    failed on macOS for 8 forms (nothrow and aligned), then ASan stopped
    with the CI error, so the old "0 allocations" check could not see those
    forms at all. With the fix the reserved FastBook still makes 0 heap
    allocations in 1,000,000 operations, now with every form counted.
  - The `debug-sanitize` test preset sets `ASAN_OPTIONS=alloc_dealloc_mismatch=1`,
    so macOS and `scripts/check.sh` run the same check as Linux. All 151
    tests pass with it.
  - Long randomized tests: CI sets `TICKFORGE_SHORT_TESTS=1` and runs 20 of
    the 150 naive-vs-queue streams and 4 of the 12 look-ahead streams, with
    the same checks. The setting is printed in the output.
    `scripts/check.sh` unsets the variable, so every commit is still gated on
    the full run. Before this, the naive-vs-queue test took 35 s (Ubuntu) and
    50 s (macOS) under the sanitizer in CI.
  - CI now prints every skipped test with its reason, and pytest runs with
    `-rs`, so tests that need `data/` report "needs data/…" instead of only
    "Skipped".

## 2026-11-28 — Market-making study (Step 4): design (locked before any Step 4 run)

Written and committed before the strategies exist and before any Step 4
configuration has been run, so no Step 4 result can shape it. One earlier set
of results has been seen: the Step 3 demonstration (Rule L, `JoinBestQuoter`
on COLB and MSFT). It is logged as trial 1 in `research/trials.jsonl`, and
nothing below was changed because of it. Where a choice below equals a Rule L
choice (order size, inventory limit, session, fees), it is because Rule L
fixed that value before its own run, not because of what the run showed.

### Rule M — what is run (trial 2: one pre-registered grid, 80 configurations)
- Data: `data/01302019_AAPL_MSFT_INTC_COLB.tfc` (30 Jan 2019). Symbols: the 4
  from Step 1, **AAPL, MSFT, INTC, COLB**.
- Grid: strategy {symmetric, Avellaneda-Stoikov} × symbol {4} × fill model
  {naive "filled on touch", queue-aware} × latency {0, 10 µs, 100 µs, 1 ms,
  10 ms} = 80 runs. Every one is run once and every one is reported.
- Latency L is the one-way delay between the strategy and the exchange, applied
  in **both directions**: order entry = L and market data = L, so the time
  from an exchange event to our reaction arriving at the exchange is 2L. Both
  directions are used because physical distance delays both (trial 1 varied
  order latency only).
- Session: the simulator starts at 09:30:00.000 ET and ends at 15:55:00.000.
  Orders are sent only from **09:35:00 to 15:55:00** (continuous trading, away
  from the 09:30 opening and 16:00 closing crosses). From 09:30 to 09:35 the
  strategies only observe (the Avellaneda-Stoikov estimators below need a
  history). At 15:55 every order is cancelled and P&L is marked at the real
  midpoint, with no flattening trade; the end inventory is reported.
- Queue priority: reference-number order (Rule G, unchanged).
- Fees (Rule J's mechanism), one setting for every run: **taker fee $0.0030
  per share, maker rebate $0.0020 per share**. Why: $0.0030 is the Reg NMS
  Rule 610 cap on access fees for stocks at or above $1, which Nasdaq charged
  for removing liquidity. Nasdaq's 2019 rebates for adding displayed liquidity
  were tiered by a member's volume, up to $0.00305. $0.0020 is a middle value.
  A new participant would likely get less; the exact 2019 base tier was not
  verified (a web search on 2026-11-28 did not find the archived price list).
  Both strategies only post (post-only), so in practice only the rebate
  applies. Every run also reports P&L **before fees**, so any other rebate can
  be applied by hand: P&L(rebate ρ) = gross P&L + ρ × filled shares.
- Tick: $0.01 (all four stocks trade above $1; Reg NMS Rule 612).
- No look-ahead: both strategies see only what the simulator delivers (Rule I).
  Every estimator below uses only data the strategy has already received.
- Simulator change needed for the markout curves (made with tests before the
  run): markout horizons become a list of up to 8 values (default unchanged:
  100 ms, 1 s, 10 s). This study uses **0, 1 ms, 10 ms, 100 ms, 1 s, 10 s,
  60 s**. Horizon 0 = the midpoint after every real message stamped at or
  before the fill time.

### Rule N — strategy 1: symmetric quoter, one tick inside the spread
Parameters and why:
- **Order size Q = 100 shares**: one round lot, the smallest order that counts
  toward the displayed quote (odd lots did not, in 2019).
- **Inventory limit L_inv = 500 shares** (5 round lots): small next to these
  stocks' displayed depth, so the no-market-impact assumption (Rule H) stays
  plausible, and the same as Rule L so trial 1 is comparable.
- Prices, from the strategy's own view of the best bid b and best ask a: bid
  = b + 1 tick if b + 1 tick < a − 1 tick, else b; ask = a − 1 tick under the
  same condition, else a. So with a spread of 3 ticks or more both sides
  improve by one tick; with 1 or 2 ticks both join the best (improving both
  sides of a 2-tick spread would put both orders on the same price, a
  self-cross). It is symmetric: inventory never moves the prices.
- Sizes: buy size = min(Q, L_inv − position), sell size = min(Q, L_inv +
  position), using the position as reported to the strategy. A side whose
  size would be ≤ 0 is not quoted. With latency, fills not yet reported can
  still take the measured inventory past ±500; that is reported, not hidden.
- Mechanics (same as `JoinBestQuoter`): post-only orders, one per side; when a
  side's target price changes, replace (losing queue priority). After a
  rejection, wait for the next market event before quoting that side again.
  No quotes, and resting quotes cancelled, when the book is one-sided or the
  trading state is not `T`.

### Rule O — strategy 2: Avellaneda-Stoikov (2008) market maker
Formulas (money in $, inventory q in shares, time in seconds):
- reservation price r = s − q · γ · σ² · (T − t)
- total spread δ = γ · σ² · (T − t) + (2/γ) · ln(1 + γ/k)
- bid = r − δ/2, ask = r + δ/2

Inputs and parameters, fixed now:
- s = the strategy's own midpoint; t = its clock; **T = 15:55:00** (the
  session end, as in the paper's finite horizon). The inventory skew
  therefore shrinks during the day and is 0 at 15:55; that is the textbook
  model's behaviour and is kept.
- **σ² (per second), estimated causally**: every second from 09:30:01 the
  strategy samples its own midpoint; σ² = mean of the squared changes between
  consecutive samples over the last 1,800 changes (30 minutes; fewer early in
  the day). A sample with a one-sided book is skipped and the change across
  it is not used. Why 30 minutes: long enough to be stable for a slow stock,
  short enough to follow the day's changing volatility.
- **k, estimated causally**: in the model, fill intensity at distance δ from
  the mid falls as A·e^(−kδ). If execution distances from the mid are
  exponential, the maximum-likelihood estimate is 1/k = their mean. So 1/k =
  the mean of |execution price − the strategy's midpoint just before it| over
  every displayed execution it has seen since 09:30:00 (`E`, and `C` with
  printable `Y`; crosses, `C` printable `N` and hidden `P` trades are left
  out). The code uses m = 1/k directly: ln(1 + γ/k) = ln(1 + γ·m).
- **γ = 4.56 × 10⁻⁵ per $** (risk aversion), fixed for every stock and chosen
  without these data: for a "typical" large-cap stock (price $100, daily
  volatility 1.5%, so σ² = 1.5²/23,400 $²/s), holding one round lot (100
  shares) at 09:35 moves the reservation price by one tick:
  γ = 0.01 / (100 × 1.5²/23,400 × 22,800). The full limit (500) then moves it
  5 ticks at 09:35. Intent: the skew should start to matter at one lot and
  reach several spreads at the limit, so inventory is pushed back well before
  the hard limit. One γ in $ means the same dollar risk aversion for every
  stock: a cheaper or calmer stock gets a smaller skew.
- Rounding to the tick, always away from the mid: bid = the nearest tick at
  or below (ties go down), ask = the nearest at or above (ties go up). δ/2 is
  at least half a tick, so bid < ask always. Post-only: bid is capped at a −
  1 tick and ask raised to at least b + 1 tick. The quotes may improve on the
  best price.
- Quoting starts only once there are ≥ 60 σ changes and ≥ 20 executions for
  k; before that, no quotes.
- Q = 100, L_inv = 500 as a hard limit, sizes and mechanics exactly as in Rule N.
  Requotes on every delivered event and on the 1-second timer.

### Rule P — outputs (`results/mm_study/`, CSV + charts) and metrics
- `runs.csv`: one row per configuration with every metric below.
- Headline table (latency 0, per strategy × symbol × fill model): P&L at 15:55
  (with rebates), number of fills, **fill rate = filled shares ÷ shares of
  accepted orders**, average 1 s markout (share-weighted; both the mid move,
  side × (mid at t+1 s − mid just before the fill), and the markout vs fill
  price, side × (mid at t+1 s − fill price)), **maximum inventory = max |inventory|**,
  **P&L per share = P&L ÷ filled shares**. Also reported: gross P&L before fees,
  end inventory, time-weighted mean |inventory| over 09:35–15:55, share of
  quotes that improved the strategy's best price.
- **Invented profit** = P&L(naive) − P&L(queue-aware), same strategy, symbol
  and latency, at every latency.
- **Latency curve**: queue-aware P&L and fill rate against latency, per
  strategy and symbol (chart + CSV).
- **Markout curves**: share-weighted markout (mid move, and vs fill price)
  against horizon 0 to 60 s, per strategy, symbol and fill model, at latency 0
  (chart + CSV with every latency).
- Validity checks for every run: 0 look-ahead violations, 0 `ahead`
  underflows, cash and inventory rebuilt from the fills equal the simulator's.
  Queue anomalies are counted and reported. A run failing a check is reported
  as invalid, never dropped.
- One day and four stocks: no significance tests and no claims beyond these
  runs. Every number is measured; none is estimated.
- Power: the run records power source and Low Power Mode at start and end
  and refuses to start off mains (no timing is reported, but the rule asks
  for it).

### Rule Q — trials log and changes after the run
- `research/trials.jsonl`: one JSON line per configuration whose results were
  looked at: trial number, configuration id, date, commit, strategy and every
  parameter, symbol, fill model, latencies, fees, session, data file, results
  file, and headline outcomes. Trial 1 = the 12 Rule L runs. Trial 2 = the 80
  runs above; the study tool appends them itself when it writes results, and
  refuses to reuse a trial number, so a rerun cannot overwrite a trial.
- No parameter is tuned after seeing results. A change that looks justified is
  written here as a *proposed* new trial, with its reason, and is not run in
  this step. If a bug is found after the run, the fix is a new trial number,
  and both trials stay in the log and the report.

### Hypotheses (written before running)
1. Invented profit > 0 (naive P&L above queue-aware) for every strategy and
   symbol at latency 0.
2. Naive fills at least as many shares as queue-aware in every one of the 40
   pairs. (Not guaranteed: these strategies react to their fills.)
3. Queue-aware P&L at 10 ms is below queue-aware P&L at 0 for every strategy
   and symbol.
4. Queue-aware 1 s mid-move markouts are negative (adverse selection) in every
   cell at latency 0.
5. Avellaneda-Stoikov has a smaller time-weighted mean |inventory| than the
   symmetric quoter for every symbol and fill model at latency 0.
6. On AAPL, MSFT and INTC the symmetric quoter improves the price on under 10%
   of its quotes (spreads are mostly one tick); on COLB on over 50%.

## 2026-11-28 — Market-making study (Step 4): build log and results (trial 2)

Built (commit `1b35aa8`, before the run): `include/tickforge/sim/mm_strategies.hpp`,
`src/sim/mm_strategies.cpp` (`TwoSidedQuoter`, `SymmetricInsideQuoter`,
`AvellanedaStoikov`, pure `as_quote`), markout horizons as a list of up to 8,
`tools/mm_study` (grid, CSVs, report, trials log), `tools/mm_study_charts.py`,
`tools/run_mm_study.sh`. 16 new tests (167 C++ tests pass in release and in
ASan + UBSan; 17 Python tests pass). One bug was found by a unit test before
any study run: when the market moved, a new bid could reach the exchange
before our own old offer at the same price had been moved, and was rejected
as a self-cross. The quoter now sends the side that is in the way first.

Run: `tools/run_mm_study.sh 2`, commit `1b35aa8`, `AC Power`, Low Power Mode
off at start and end (`results/mm_study/run_log.txt`). **80 of 80 runs valid**:
0 look-ahead violations, 0 `ahead` underflows, accounting rebuilt from fills
matches, 0 queue anomalies. Trial 2 is in `research/trials.jsonl` (80 lines).
Results: `results/mm_study/` (`report.md`, `runs.csv`, `headline_latency0.csv`,
`invented_profit.csv`, `latency_curve.csv`, `markouts.csv`, 4 PNG charts).

After the run, two cosmetic fixes, no number changed: the report's markout
header said "1000 ms / 10000 ms / 60000 ms" (corrected to 1 s / 10 s / 60 s
in the file and in the tool), and two overlapping end labels in the latency
chart were moved apart (charts regenerated from the same CSVs).

Hypotheses (written before the run):
1. "Invented profit > 0 for every strategy and symbol at latency 0":
   **refuted, 5 of 8.** Negative for symmetric AAPL (−$2,432.08), symmetric
   COLB (−$83.10) and Avellaneda-Stoikov COLB (−$46.25).
2. "Naive fills at least as many shares as queue-aware in all 40 pairs":
   **confirmed, 40 of 40.**
3. "Queue-aware P&L at 10 ms below latency 0": **6 of 8**; both COLB cells
   went the other way (symmetric −$135.00 → −$52.31; A-S $51.95 → $103.19).
4. "Queue-aware 1 s mid-move markouts negative at latency 0": **confirmed, 8
   of 8**, and negative in all 80 runs. Negative already at horizon 0 in all 80.
5. "A-S has lower time-weighted mean |inventory| than symmetric": **confirmed,
   8 of 8** (e.g. AAPL queue-aware 49.8 vs 295.1 shares).
6. "Symmetric improves the price on < 10% of quotes for AAPL, MSFT, INTC and
   > 50% for COLB": **refuted for AAPL** (46.1% naive, 50.3% queue-aware);
   holds for MSFT (4.8%, 5.5%), INTC (2.8%, 3.0%) and COLB (67.2%, 71.9%).

Other measured facts (descriptive, looked at after the run):
- Net P&L was negative in 59 of 80 runs, gross P&L (before the rebate) in 72
  of 80. The 1 s markout against the fill price was negative in 77 of 80: the
  price moved against the fill by more than the half-spread earned.
- P&L per filled share was higher under naive than queue-aware in 7 of 8
  latency-0 cells (not A-S COLB). Reading (fits the numbers, not tested): the
  naive model always flatters the *per-share edge*, but it also inflates
  *volume*; where the edge is negative even under naive (symmetric AAPL:
  −$0.00245 per share on 3.69 M shares vs −$0.00456 on 1.45 M), more volume
  means a bigger loss, so invented profit is negative.
- COLB P&Ls are tens of dollars and every COLB run ended with |inventory|
  300-500 marked at the 15:55 mid, so a few cents of COLB price move decide
  their sign. They should not be read as an edge.
- Queue-aware fill rate *rose* with latency for most cells (symmetric MSFT
  17.32% → 26.38% at 1 ms), while fills from real executions fell and fills
  from a real order arriving at our price rose (357 → 7,252) and the
  half-spread captured fell (0.527¢ → 0.239¢). Same pattern as trial 1:
  stale quotes get picked off.
- Queue-aware inventory passed the 500 limit in 20 of 40 queue-aware runs,
  up to 590, including at latency 0. At latency 0 this comes from events at
  the same nanosecond: a fill and our replacement order (sized from the
  position before that fill) reach the exchange at the same timestamp.

Proposed new trials (not run; each would get its own trial number):
- P1. Same grid on more days. One day cannot separate an edge from noise;
  this is the main limit of every number above.
- P2. Report a flattened P&L as well (close the end inventory at 15:55 by
  crossing the spread and paying the taker fee), so stocks like COLB are not
  decided by the end-of-day mark.
- P3. Size orders from the position *plus* open orders on that side, to stop
  the inventory overshoot. This changes the strategy, so it is a new trial,
  not a fix to trial 2.

## Open questions

- Target platform/toolchain versions to pin in CI (currently using whatever
  `ubuntu-latest` / `macos-latest` ship with).
