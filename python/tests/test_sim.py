"""The C++ simulator driven from Python (PROGRESS.md, Rule K)."""

import struct
from pathlib import Path

import pytest

import tickforge as tf
from tickforge.strategies import JoinBestQuoterPy

LOC = 7
SYM = b"ABC     "
DATA = Path(__file__).resolve().parents[2] / "data" / "01302019_AAPL_MSFT_INTC_COLB.tfc"


def _hdr(kind, ts):
    return kind + struct.pack(">HH", LOC, 0) + ts.to_bytes(6, "big")


def directory(ts):
    return _hdr(b"R", ts) + SYM + b"QN" + struct.pack(">I", 100) + b"NC" + b"Z " + b"PNN1N" + struct.pack(">I", 0) + b"N"


def add(ts, ref, side, shares, price):
    return _hdr(b"A", ts) + struct.pack(">QcI", ref, side, shares) + SYM + struct.pack(">I", price)


def executed(ts, ref, shares):
    return _hdr(b"E", ts) + struct.pack(">QIQ", ref, shares, 1)


def write_itch(path, msgs):
    with open(path, "wb") as f:
        for m in msgs:
            f.write(struct.pack(">H", len(m)) + m)


def test_message_sizes_match_the_spec():
    assert len(directory(1)) == 39
    assert len(add(1, 1, b"B", 100, 100000)) == 36
    assert len(executed(1, 1, 100)) == 31


class BuyAt(tf.Strategy):
    """Places one order at a fixed time and records what it is told."""

    def __init__(self, at_ns, price, qty):
        super().__init__()
        self.at_ns, self.price, self.qty = at_ns, price, qty
        self.fills, self.updates, self.id = [], [], 0

    def on_start(self, ctx):
        ctx.set_timer(self.at_ns)

    def on_timer(self, ctx, timer_id):
        self.id = ctx.place(tf.Side.buy, self.price, self.qty)

    def on_order(self, ctx, u):
        self.updates.append(u.kind)

    def on_fill(self, ctx, f):
        self.fills.append((f.exchange_ts, f.qty, f.price))


def test_python_strategy_waits_for_500_shares_ahead(tmp_path):
    p = tmp_path / "day.itch"
    write_itch(
        p,
        [
            directory(1),
            add(10, 1, b"B", 200, 100000),
            add(20, 2, b"B", 300, 100000),
            add(25, 3, b"S", 500, 100100),
            add(150, 4, b"B", 100, 100000),  # behind the Python order
            executed(200, 1, 200),
            executed(300, 2, 300),  # 500 shares traded, all ahead of us
            executed(400, 4, 70),  # behind us: we fill first
            add(500, 5, b"S", 1, 100200),
        ],
    )
    cfg = tf.SimConfig()
    cfg.symbol = "ABC"
    cfg.pnl_sample_interval_ns = 0
    s = BuyAt(100, 100000, 100)
    r = tf.simulate(str(p), cfg, s)
    assert r.symbol_found
    assert s.updates == [tf.UpdateKind.accepted]
    assert s.fills == [(400, 70, 100000)]
    assert len(r.fills) == 1 and r.fills[0].trigger == "E"
    assert r.fills[0].report.liquidity == tf.Liquidity.maker
    assert r.final_inventory == 70
    assert r.final_cash == -70 * 100000 * 100 + 70 * 2000
    assert r.stats.lookahead_violations == 0

    cfg.model = tf.FillModel.naive_touch
    naive = tf.simulate(str(p), cfg, BuyAt(100, 100000, 100))
    assert [(f.report.exchange_ts, f.report.qty) for f in naive.fills] == [(200, 100)]


def test_exceptions_in_python_callbacks_reach_the_caller(tmp_path):
    p = tmp_path / "day.itch"
    write_itch(p, [directory(1), add(10, 1, b"B", 100, 100000), add(20, 2, b"S", 100, 100100)])

    class Boom(tf.Strategy):
        def on_market(self, ctx, ev):
            raise ValueError("boom")

    cfg = tf.SimConfig()
    cfg.symbol = "ABC"
    with pytest.raises(ValueError, match="boom"):
        tf.simulate(str(p), cfg, Boom())


@pytest.mark.skipif(not DATA.exists(), reason=f"needs {DATA.name} in data/ (not in git or CI; see data/README.md)")
def test_python_and_cpp_join_best_quoter_agree_on_colb():
    hour, minute = 3_600_000_000_000, 60_000_000_000
    cfg = tf.SimConfig()
    cfg.symbol = "COLB"
    cfg.order_latency_ns = 100_000
    cfg.start_ns = 9 * hour + 35 * minute
    cfg.end_ns = 15 * hour + 55 * minute
    cpp = tf.simulate(str(DATA), cfg, tf.JoinBestQuoter(100, 500))
    py = tf.simulate(str(DATA), cfg, JoinBestQuoterPy(100, 500))
    assert len(cpp.decisions) > 100
    assert cpp.decisions == py.decisions
    key = lambda f: (f.report.id, f.report.exchange_ts, f.report.price, f.report.qty, f.report.side)
    assert [key(f) for f in cpp.fills] == [key(f) for f in py.fills]
    assert cpp.final_mtm == py.final_mtm
