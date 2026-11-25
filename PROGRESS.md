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

## Open questions

- Which exact ITCH 5.0 sample date/file will be used for the first replay
  test, and where it will be sourced from.
- Target platform/toolchain versions to pin in CI (currently using whatever
  `ubuntu-latest` / `macos-latest` ship with).
