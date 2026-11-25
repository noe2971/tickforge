"""Tests for tools/select_less_liquid.py (PROGRESS.md Rule A)."""

import csv
import importlib.util
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "select_less_liquid.py"
spec = importlib.util.spec_from_file_location("select_less_liquid", SCRIPT)
sel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sel)

TYPES = list("SRHYLVWKJhAFECXDUPQBINO")
FIELDS = ["locate", "symbol", "market_category", "issue_classification", "etp_flag",
          "authenticity", "round_lot_size", "closing_cross_price", "total"] + TYPES


def row(sym, executions, **kw):
    r = {k: "0" for k in FIELDS}
    r.update(locate="1", symbol=sym, market_category="Q", issue_classification="C",
             etp_flag="N", authenticity="P", round_lot_size="100",
             closing_cross_price="20.0000", total="1", E=str(executions))
    r.update(kw)
    return r


def run(tmp_path, rows, capsys):
    path = tmp_path / "by_symbol.csv"
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    rc = sel.main(str(path))
    return rc, capsys.readouterr().out.strip().splitlines()


def test_picks_closest_to_2000_executions(tmp_path, capsys):
    rc, out = run(tmp_path, [row("AAA", 100), row("BBB", 1900), row("CCC", 2500)], capsys)
    assert rc == 0
    assert out[-1] == "BBB"


def test_ties_go_to_alphabetically_first(tmp_path, capsys):
    rc, out = run(tmp_path, [row("ZZZ", 2100), row("MMM", 1900)], capsys)
    assert out[-1] == "MMM"


def test_executions_count_e_plus_c(tmp_path, capsys):
    rc, out = run(tmp_path, [row("AAA", 1000, C="1000"), row("BBB", 1500)], capsys)
    assert out[-1] == "AAA"


@pytest.mark.parametrize("override", [
    {"market_category": "N"},          # NYSE-listed
    {"issue_classification": "Q"},     # not common stock
    {"etp_flag": "Y"},                 # ETF/ETP
    {"authenticity": "T"},             # test symbol
    {"round_lot_size": "10"},
    {"closing_cross_price": "4.9999"}, # penny stock
    {"closing_cross_price": ""},       # no closing cross
    {"symbol": "AAPL"},                # already chosen
])
def test_filters_exclude(tmp_path, capsys, override):
    rc, out = run(tmp_path, [row("GOOD", 10), row("BAD", 2000, **override)], capsys)
    assert out[-1] == "GOOD"


def test_no_eligible_stock_fails(tmp_path, capsys):
    rc, _ = run(tmp_path, [row("BAD", 2000, market_category="N")], capsys)
    assert rc == 1
