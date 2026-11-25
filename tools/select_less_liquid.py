#!/usr/bin/env python3
"""Pick the less-liquid Nasdaq stock mechanically (PROGRESS.md, "Rule A").

Usage: python3 -I tools/select_less_liquid.py results/itch_01302019_counts_by_symbol.csv

Reads the per-symbol count CSV written by tools/itch_count and applies, in
order (the rule was committed before any data was looked at):
  1. market category Q, G or S; issue classification C; ETP flag not Y;
     authenticity P; round lot size 100;
  2. a closing cross at >= $5.00;
  3. not AAPL, MSFT or INTC;
  4. order executions (E + C) closest to 2,000; ties -> alphabetical.
Prints the chosen symbol on the last line, preceded by the shortlist.
Standard library only.
"""

import csv
import sys

TARGET_EXECUTIONS = 2000
EXCLUDED = {"AAPL", "MSFT", "INTC"}


def main(path: str) -> int:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    eligible = []
    for r in rows:
        if r["market_category"] not in {"Q", "G", "S"}:
            continue
        if r["issue_classification"] != "C" or r["etp_flag"] == "Y":
            continue
        if r["authenticity"] != "P" or r["round_lot_size"] != "100":
            continue
        if not r["closing_cross_price"] or float(r["closing_cross_price"]) < 5.0:
            continue
        if r["symbol"] in EXCLUDED:
            continue
        executions = int(r["E"]) + int(r["C"])
        eligible.append((abs(executions - TARGET_EXECUTIONS), r["symbol"], executions, r))

    if not eligible:
        print("no stock satisfies Rule A", file=sys.stderr)
        return 1

    eligible.sort(key=lambda t: (t[0], t[1]))
    print(f"{len(eligible)} stocks pass filters 1-3; closest to {TARGET_EXECUTIONS} executions:")
    print("symbol,executions(E+C),adds(A+F),total_messages,closing_cross_price,market_category")
    for _, sym, ex, r in eligible[:10]:
        print(f"{sym},{ex},{int(r['A']) + int(r['F'])},{r['total']},{r['closing_cross_price']},{r['market_category']}")
    print(eligible[0][1])
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
