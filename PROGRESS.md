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

## Open questions

- Target platform/toolchain versions to pin in CI (currently using whatever
  `ubuntu-latest` / `macos-latest` ship with).
