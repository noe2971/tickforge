#!/usr/bin/env bash
# One command for the whole ITCH data-layer pipeline, once data is downloaded:
#   1. download + verify the sample day (resumes if interrupted)
#   2. full-day message-count report -> results/
#   3. pick the less-liquid stock by PROGRESS.md Rule A
#   4. write the compact per-symbol file -> data/
#   5. full-day parse-throughput benchmark -> results/benchmarks.md
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
day="01302019"
gz="data/${day}.NASDAQ_ITCH50.gz"

tools/download_itch.sh "${day}.NASDAQ_ITCH50.gz"

cmake --preset release > /dev/null
cmake --build --preset release -j "$(getconf _NPROCESSORS_ONLN)" > /dev/null

echo "== Counting messages"
build/release/tools/itch_count "$gz" results "$day"

echo "== Selecting the less-liquid stock (Rule A)"
python3 -I tools/select_less_liquid.py "results/itch_${day}_counts_by_symbol.csv" | tee "results/itch_${day}_less_liquid_selection.txt"
pick="$(tail -1 "results/itch_${day}_less_liquid_selection.txt")"

echo "== Filtering AAPL MSFT INTC $pick"
build/release/tools/itch_filter "$gz" "data/${day}_AAPL_MSFT_INTC_${pick}.tfc" AAPL MSFT INTC "$pick" \
    | tee "results/itch_${day}_filter_summary.txt"

echo "== Benchmark"
tools/run_benchmarks.sh "$gz"

scripts/ignore-large-results.sh
echo "Done. Review results/ and commit."
