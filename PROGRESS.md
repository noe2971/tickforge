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

Blocked:
- The network on this machine dropped from ~5 MB/s to ~10 KiB/s during the
  download (a CDN unrelated to Nasdaq was equally slow, so it is the local
  link). The full-day count report, the Rule A stock choice, the filtered
  file and `results/benchmarks.md` all need the complete file and have **not**
  been produced. Once the download completes, run `tools/build_data_layer.sh`.

Process note:
- Commit `f4999e2` was made after a test run that reported 3 failures. They
  came from a stale sanitizer binary still containing the planted bug (the
  source had already been restored); a clean rebuild passed 52/52. The commit
  command had checked ctest's text instead of its exit code. From now on every
  commit goes through `scripts/check.sh`, which rebuilds and fails on ctest's
  exit status (verified: it exits non-zero with the bug planted).
- Commit `f6f49d9` (docs only) was made without running the tests first.

## Open questions

- Target platform/toolchain versions to pin in CI (currently using whatever
  `ubuntu-latest` / `macos-latest` ship with).
