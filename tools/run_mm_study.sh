#!/usr/bin/env bash
# Runs the Step 4 market-making study (PROGRESS.md, Rules M-Q) end to end:
# release build, power check (mains only, recorded at start and end), the
# 80-configuration grid, charts.
#
#   tools/run_mm_study.sh <trial_number>
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
source tools/bench_env.sh

trial="${1:?usage: tools/run_mm_study.sh <trial_number>}"
out="results/mm_study"
data="data/01302019_AAPL_MSFT_INTC_COLB.tfc"
[[ -f "$data" ]] || { echo "missing $data (see data/README.md)" >&2; exit 1; }

require_mains
commit="$(git rev-parse --short HEAD)"
if [[ -n "$(git status --porcelain -- src include tools CMakeLists.txt)" ]]; then commit="${commit}-dirty"; fi

cmake --preset release > /dev/null
cmake --build --preset release --target mm_study -j "$(getconf _NPROCESSORS_ONLN)" > /dev/null

mkdir -p "$out"
start_state="$(power_state)"; start_detail="$(power_detail)"; start_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
build/release/tools/mm_study "$data" "$out" research/trials.jsonl "$trial" "$commit"
end_state="$(power_state)"; end_detail="$(power_detail)"; end_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

{
    echo "trial: $trial"
    echo "commit: $commit"
    echo "start: $start_time  $start_state  ($start_detail)"
    echo "end:   $end_time  $end_state  ($end_detail)"
    if [[ "$start_state" == "$end_state" ]]; then echo "power unchanged: yes"; else echo "power unchanged: NO"; fi
} > "$out/run_log.txt"
cat "$out/run_log.txt"

uv run --project python --extra analysis python tools/mm_study_charts.py "$out"
