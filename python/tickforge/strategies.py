"""Strategies written in Python on top of the C++ simulator.

JoinBestQuoterPy is a line-by-line Python copy of the C++ JoinBestQuoter
(include/tickforge/sim/strategies.hpp). The test suite runs both on the same
data and requires identical decisions and fills, which checks the bindings.
"""

from ._tickforge import OrderStatus, Side, Strategy, UpdateKind

_FINISHED = {OrderStatus.filled, OrderStatus.cancelled, OrderStatus.rejected, OrderStatus.replaced}


class _Slot:
    __slots__ = ("id", "price", "cancelling", "blocked")

    def __init__(self, blocked=False):
        self.id = 0
        self.price = 0
        self.cancelling = False
        self.blocked = blocked


class JoinBestQuoterPy(Strategy):
    """One post-only order of `size` at the best bid and one at the best ask."""

    def __init__(self, size=100, max_inventory=500):
        super().__init__()
        self.size = size
        self.max_inv = max_inventory
        self.slots = {Side.buy: _Slot(), Side.sell: _Slot()}

    def on_start(self, ctx):
        self._requote(ctx)

    def on_market(self, ctx, ev):
        for sl in self.slots.values():
            sl.blocked = False
        self._requote(ctx)

    def on_order(self, ctx, u):
        for side, sl in self.slots.items():
            if sl.id != u.id:
                continue
            if u.kind == UpdateKind.rejected:
                self.slots[side] = _Slot(blocked=True)
            elif u.kind == UpdateKind.cancelled and u.open_qty == 0:
                self.slots[side] = _Slot()
                self._requote(ctx)

    def on_fill(self, ctx, f):
        self._requote(ctx)

    def _requote(self, ctx):
        self._requote_side(ctx, Side.buy)
        self._requote_side(ctx, Side.sell)

    def _requote_side(self, ctx, side):
        sl = self.slots[side]
        if sl.id != 0:
            k = ctx.order(sl.id)
            if k is None or k.status in _FINISHED:
                sl = self.slots[side] = _Slot(blocked=sl.blocked)
        best = ctx.best_bid() if side == Side.buy else ctx.best_ask()
        pos = ctx.position()
        allowed = pos < self.max_inv if side == Side.buy else pos > -self.max_inv
        if best is None or not allowed:
            if sl.id != 0 and not sl.cancelling:
                ctx.cancel(sl.id)
                sl.cancelling = True
            return
        if sl.cancelling or sl.blocked:
            return
        if sl.id == 0:
            sl.id = ctx.place(side, best.price, self.size, post_only=True)
            sl.price = best.price
        elif sl.price != best.price:
            sl.id = ctx.replace(sl.id, best.price, self.size)
            sl.price = best.price
