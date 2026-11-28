#!/usr/bin/env bash
# Step 2 benchmarks (method: PROGRESS.md "Rule E"): full-day replay
# throughput, per-call latency, and the fast-vs-reference speedup, for all
# symbols. Refuses to run unless on mains power; records power source and Low
# Power Mode at the start and end of every run and discards a run whose power
# changed. Writes results/bench/03_book_replay.md, then rebuilds
# results/benchmarks.md.
#
# Usage: tools/run_book_benchmarks.sh [day.gz] [book_peaks.csv]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
source tools/bench_env.sh
input="${1:-data/01302019.NASDAQ_ITCH50.gz}"
peaks="${2:-results/itch_01302019_book_peaks.csv}"
out="results/bench/03_book_replay.md"
raw_dir="results/bench/raw"
mkdir -p "$raw_dir"

require_mains
cmake --preset release > /dev/null
cmake --build --preset release --clean-first -j "$(getconf _NPROCESSORS_ONLN)" > /dev/null
commit="$(git rev-parse --short HEAD)$(git diff --quiet || echo '-dirty')"

run() {  # engine mode
    local engine="$1" mode="$2" f="$raw_dir/${1}_${2}.md" start end
    require_mains
    start="$(power_state)"
    local dstart dend
    dstart="$(power_detail)"
    echo "== $engine $mode ($start)" >&2
    build/release/bench/book_replay "$input" "$peaks" --engine "$engine" --mode "$mode" --passes 3 --warmup 1 > "$f"
    end="$(power_state)"
    dend="$(power_detail)"
    if [[ "$start" != "$end" ]]; then
        echo "Power changed during $engine $mode ('$start' -> '$end'); result discarded, run again." >&2
        exit 3
    fi
    echo "start $start, $dstart | end $end, $dend" > "$raw_dir/${engine}_${mode}.power"
}
value() { sed -n "s/^<!-- $2=\(.*\) -->$/\1/p" "$raw_dir/$1.md"; }

for engine in fast reference; do
    for mode in throughput latency; do run "$engine" "$mode"; done
done

d_fast="$(value fast_throughput digest)"
d_ref="$(value reference_throughput digest)"
if [[ "$d_fast" != "$d_ref" || "$d_fast" != "$(value fast_latency digest)" || "$d_ref" != "$(value reference_latency digest)" ]]; then
    echo "Book hashes differ between engines; benchmark invalid." >&2
    exit 4
fi
speedup="$(awk -v r="$(value reference_throughput median_book_s)" -v f="$(value fast_throughput median_book_s)" 'BEGIN { printf "%.2f", r / f }')"

strip() { grep -v '^<!--' "$raw_dir/$1.md"; }
{
    echo "## Order book replay — full day, all symbols (Step 2)"
    echo
    echo "Method (locked in \`PROGRESS.md\`, Rule E, before the first run): books pre-created and pre-sized"
    echo "from \`$peaks\` (measured peaks of the validation run); each pass inflates 1 GiB of whole"
    echo "frames into memory, then frames, decodes and applies every message in it to the books. \"Book s\""
    echo "is the sum of those framing + decode + book-update segments; inflate is reported separately."
    echo "1 warm-up pass (discarded) + 3 measured passes per engine; median by book time. Heap allocations"
    echo "are counted inside the book segments only."
    echo
    echo "Command: \`tools/run_book_benchmarks.sh $input $peaks\` (run on $(date -u +%Y-%m-%d), commit \`$commit\`)"
    echo
    echo "| Machine | |"
    echo "|---|---|"
    machine_table
    for k in fast_throughput fast_latency reference_throughput reference_latency; do
        echo "| Power, ${k/_/ } run (start; end) | $(sed 's/ | /; /' "$raw_dir/$k.power") |"
    done
    echo
    echo "Both engines had identical books at every 1 GiB chunk boundary and at the end of the day (combined hash \`$d_fast\`)."
    echo
    echo "### Throughput"
    echo
    echo "**Speedup of FastBook over ReferenceBook (median book time): ${speedup}x.**"
    echo
    echo "FastBook:"
    echo
    strip fast_throughput
    echo
    echo "ReferenceBook:"
    echo
    strip reference_throughput
    echo
    echo "### Per-call latency (add, cancel, delete, execute, replace)"
    echo
    echo "Every book call timed on its own; values are whole clock ticks converted to ns, so each"
    echo "percentile is accurate to +/- one tick. The timer's own cost is included, not subtracted."
    echo
    echo "| Call | FastBook p50 | p99 | p99.9 | ReferenceBook p50 | p99 | p99.9 |"
    echo "|---|---:|---:|---:|---:|---:|---:|"
    names=("add (A+F)" "execute (E+C)" "cancel (X)" "delete (D)" "replace (U)")
    for k in 0 1 2 3 4; do
        printf '| %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |\n' "${names[$k]}" \
            "$(value fast_latency k${k}_p50)" "$(value fast_latency k${k}_p99)" "$(value fast_latency k${k}_p999)" \
            "$(value reference_latency k${k}_p50)" "$(value reference_latency k${k}_p99)" "$(value reference_latency k${k}_p999)"
    done
    echo
    echo "(ns, median over 3 passes.) Every pass, FastBook:"
    echo
    strip fast_latency
    echo
    echo "Every pass, ReferenceBook:"
    echo
    strip reference_latency
} > "$out"
echo "Wrote $out" >&2
tools/assemble_benchmarks.sh
