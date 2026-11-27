# Tickforge

A C++20 limit order book engine that replays a full day of real Nasdaq
TotalView-ITCH 5.0 data, plus a queue-aware, latency-aware simulator for
testing market-making strategies honestly. Python is used only for analysis
and plots, through pybind11 bindings.

## Layout

- `include/tickforge/` — public headers
- `src/` — engine implementation
- `tests/` — GoogleTest suite (FetchContent)
- `bench/` — Google Benchmark suite (FetchContent)
- `python/` — pybind11 bindings, built with scikit-build-core, managed with uv
- `tools/` — standalone CLI utilities
- `data/` — raw/derived market data (git-ignored; see `data/README.md`)
- `results/` — measured results (tracked; files over 5 MB are kept out)
- `docs/` — design notes and documentation

## Building

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Other presets: `debug`, `debug-sanitize` (AddressSanitizer + UndefinedBehaviorSanitizer).

## Git hooks

```bash
./scripts/install-hooks.sh
```

Installs a `commit-msg` hook that strips AI-attribution lines from commit
messages.

## Order book engine

Two engines behind one interface (`include/tickforge/book/`): `FastBook`
(memory pools, intrusive order queues, sorted level arrays, open-addressing id
map) and `ReferenceBook` (`std::map` + `std::list`, written for clarity). Both
replay Nasdaq ITCH (add / execute / cancel / delete / replace) and match new
limit and market orders with price-time priority. Why each structure was
chosen: `docs/order_book_design.md`.

```bash
build/release/tools/book_fuzz                                  # differential fuzz, fast vs reference
build/release/tools/itch_validate data/01302019.NASDAQ_ITCH50.gz   # real-day replay validation
build/release/tools/itch_engine_diff data/01302019.NASDAQ_ITCH50.gz
build/release/tools/itch_top_levels data/01302019.NASDAQ_ITCH50.gz AAPL 10:00:00 --levels 10
tools/run_book_benchmarks.sh                                   # needs mains power
```

`itch_top_levels` prints the book as it stood just before the given time
(nothing stamped at or after it is applied).

See `CLAUDE.md` for project rules and `PROGRESS.md` for session notes.
