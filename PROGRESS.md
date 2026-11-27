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

## Open questions

- Target platform/toolchain versions to pin in CI (currently using whatever
  `ubuntu-latest` / `macos-latest` ship with).
