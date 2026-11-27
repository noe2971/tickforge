#!/usr/bin/env bash
# Rebuilds results/benchmarks.md from the sections in results/bench/*.md, in
# file-name order. Each section is written by the script that measured it.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
out="results/benchmarks.md"
{
    echo "# Benchmarks"
    echo
    echo "All numbers below were measured by the commands shown; none are estimates."
    echo "Every section records the power source and Low Power Mode at the start and"
    echo "the end of its run (rule in \`PROGRESS.md\`, \"Power source\")."
    for f in results/bench/*.md; do
        echo
        cat "$f"
    done
} > "$out"
echo "Wrote $out" >&2
