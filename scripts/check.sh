#!/usr/bin/env bash
# Pre-commit gate: rebuild and run the full test suite in the release and the
# sanitizer (ASan + UBSan) builds. Exits non-zero if anything fails, so use it
# as `scripts/check.sh && git commit ...`.
#
# It always rebuilds from clean first (--clean-first), so a stale test binary
# can never report on code that is no longer in the working tree. A plain
# incremental build is not enough on macOS: its GNU Make 3.81 compares file
# times to the whole second, so a file edited in the same second as the last
# build looks up to date and is silently not recompiled.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
# The local gate always runs the long randomized tests in full (CI sets
# TICKFORGE_SHORT_TESTS=1 for a smaller fixed set; see tests/test_settings.hpp).
unset TICKFORGE_SHORT_TESTS

for preset in release debug-sanitize; do
    echo "== $preset: configure + build"
    cmake --preset "$preset" > /dev/null
    cmake --build --preset "$preset" --clean-first -j "$jobs" > /dev/null
    echo "== $preset: test"
    ctest --preset "$preset" -j "$jobs" --output-on-failure | tail -n 3
    # ctest's exit status (not its text) decides; pipefail propagates it.
done
echo "All checks passed."
