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
- `results/` — generated run output (git-ignored)
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

See `CLAUDE.md` for project rules and `PROGRESS.md` for session notes.
