#!/usr/bin/env bash
# Step 1 benchmark: full-day ITCH parse throughput (method: PROGRESS.md
# "Rule B"). Builds the release preset, refuses to run unless the laptop is
# on mains power, records the power source and Low Power Mode at the start
# and the end of the run, and writes results/bench/02_parse_itch_mains.md.
# Then rebuilds results/benchmarks.md from every section in results/bench/.
#
# Usage: tools/run_benchmarks.sh [path/to/day.gz]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
source tools/bench_env.sh
input="${1:-data/01302019.NASDAQ_ITCH50.gz}"
out="results/bench/02_parse_itch_mains.md"

require_mains
cmake --preset release > /dev/null
cmake --build --preset release --clean-first -j "$(getconf _NPROCESSORS_ONLN)" --target itch_throughput > /dev/null
commit="$(git rev-parse --short HEAD)$(git diff --quiet || echo '-dirty')"

power_start="$(power_state)"
detail_start="$(power_detail)"
echo "Running parse benchmark on $input ($power_start)..." >&2
bench_out="$(build/release/bench/itch_throughput "$input" 3 1)"
power_end="$(power_state)"
detail_end="$(power_detail)"
if [[ "$power_start" != "$power_end" ]]; then
    echo "Power changed during the run ('$power_start' -> '$power_end'); result discarded, run again." >&2
    exit 3
fi

mkdir -p results/bench
{
    echo "## ITCH 5.0 full-day parse throughput — rerun on mains power"
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
    machine_table
    echo "| Power at start | $power_start; $detail_start |"
    echo "| Power at end | $power_end; $detail_end |"
    echo
    echo "$bench_out"
} > "$out"
echo "Wrote $out" >&2
tools/assemble_benchmarks.sh
