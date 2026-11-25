#!/usr/bin/env bash
# Builds the release preset, runs the full-day ITCH parse-throughput benchmark
# (method: PROGRESS.md "Rule B") and writes results/benchmarks.md with the
# machine details next to the measured numbers.
#
# Usage: tools/run_benchmarks.sh [path/to/day.gz]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
input="${1:-data/01302019.NASDAQ_ITCH50.gz}"
out="results/benchmarks.md"

cmake --preset release > /dev/null
cmake --build --preset release -j "$(getconf _NPROCESSORS_ONLN)" --target itch_throughput > /dev/null

if [[ "$(uname)" == "Darwin" ]]; then
    cpu="$(sysctl -n machdep.cpu.brand_string)"
    cores="$(sysctl -n hw.ncpu) logical ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?) performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo ?) efficiency)"
    ram="$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GiB"
    os="macOS $(sw_vers -productVersion) ($(uname -m))"
    power="$(pmset -g batt 2>/dev/null | head -1 | sed "s/Now drawing from //; s/'//g")"
else
    cpu="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    cores="$(nproc) logical"
    ram="$(awk '/MemTotal/ {printf "%d GiB", $2/1024/1024}' /proc/meminfo)"
    os="$(uname -sr) ($(uname -m))"
    power="n/a"
fi
compiler="$(c++ --version | head -1)"
flags="$(grep -E '^CMAKE_CXX_FLAGS_RELEASE:' build/release/CMakeCache.txt | cut -d= -f2-)"
commit="$(git rev-parse --short HEAD)$(git diff --quiet || echo '-dirty')"

echo "Running benchmark on $input (this takes several minutes)..." >&2
bench_out="$(build/release/bench/itch_throughput "$input" 3 1)"

{
    echo "# Benchmarks"
    echo
    echo "All numbers below were measured by the commands shown; none are estimates."
    echo
    echo "## ITCH 5.0 full-day parse throughput"
    echo
    echo "Method (locked in \`PROGRESS.md\`, Rule B, before the first run): stream"
    echo "\`$(basename "$input")\` with zlib, frame on the 2-byte length prefix, decode every"
    echo "message into its typed struct and fold the fields into a checksum. 1 warm-up"
    echo "pass (discarded), then 3 measured passes; the median row is the middle pass by"
    echo "wall time. MB = 10^6 bytes. \"Decode-only\" divides by the frame+decode time"
    echo "alone, excluding time spent inside zlib and file reads."
    echo
    echo "Command: \`tools/run_benchmarks.sh $input\` (run on $(date -u +%Y-%m-%d), commit \`$commit\`)"
    echo
    echo "| Machine | |"
    echo "|---|---|"
    echo "| CPU | $cpu |"
    echo "| Cores | $cores |"
    echo "| RAM | $ram |"
    echo "| OS | $os |"
    echo "| Power | $power |"
    echo "| Compiler | $compiler |"
    echo "| Flags | CMake Release: \`$flags\` |"
    echo
    echo "$bench_out"
} > "$out"

echo "Wrote $out" >&2
