# Simulator design (Step 3)

The rules are in `PROGRESS.md` (Rules G–L). This note explains why the code
is shaped the way it is. Code: `include/tickforge/sim/`, `src/sim/`; tests:
`tests/test_sim.cpp`, `tests/test_sim_properties.cpp`, `python/tests/test_sim.py`.

## Shadow orders

The replayed day is fixed: we can't insert our orders into Nasdaq's real
messages. A *shadow order* is tracked next to the real book. It records where
it would stand in the queue and which real events would have traded with it.
The real book is rebuilt exactly as in Step 2 and never changes. We assume no
market impact: a real order behind our filled shadow order still trades as in
the feed.

## Queue position = reference-number order

Step 2 found 7,056 executions that hit an order which was not first in our
rebuilt queue. Before building anything, `tools/itch_queue_jumps` explained
every one of them. All 8,096,995 `E` executions of the day hit the order with
the smallest *reference number* at its price. The "jumps" were orders entered
before the open whose Add message only appeared at 09:30. They kept their
entry-time priority. So the simulator ranks by reference number:

- every real order's priority key is its reference number;
- a shadow order arriving at the exchange takes key K = the largest reference
  the stock has published so far;
- `ahead` = shares of real orders at the same side and price with reference
  ≤ K. It goes down when they trade, are cancelled, deleted or replaced, and
  up when a late add with an older reference joins.

This costs one comparison per event and needs no extra map, because the
reference number arrives in every message.

## Fills

| Event | Queue-aware | Naive "filled on touch" |
|---|---|---|
| Real execution at price e on our side | fill if our price is e or better, `ahead` = 0 and the executed order is not ahead of us; the shadows together take at most the executed shares | fill everything if a print is at or through our price |
| Real order arriving on the other side at a price that crosses us | fill up to its size (it would have traded with us) | fill everything |
| Hidden-order trade (`P`), cross (`Q`) | ignored | fill everything if at or through our price |
| Our order crosses the book on arrival | takes displayed liquidity (taker), never the same real shares twice | same |

Every queue-aware trigger is also a naive trigger, and naive always fills the
whole order. So for orders that don't depend on fills, naive fills at least
as much and no later. A property test checks that on random streams and on
COLB and MSFT.

## Time, latency and no look-ahead

Real messages are processed one at a time. Everything else (our actions
reaching the exchange, deliveries to the strategy, markouts, P&L samples,
timers) waits in one event queue ordered by (time, class, sequence). Before a
real message stamped t is applied, every queued event earlier than t runs.
An event at exactly t runs after the real messages at t, so real messages win
ties, and an order arriving at the same nanosecond as a real order queues
behind it.

The strategy gets a `Context`, never the simulator. Its book is a second
`FastBook`, fed only by delivered messages (exchange time + market-data
latency). It cannot read the exchange book, its own queue position, or
anything newer than its clock allows. Every callback checks that, and a
look-ahead test changes the future (other random futures, and the COLB
afternoon altered) and requires every earlier decision to be identical.

## Money

Cash and fees are integers in micro-dollars. Prices stay in ITCH's $0.0001
ticks; one tick is 100 micro-dollars, and Nasdaq-style fees such as $0.00295
are exact. The midpoint is kept as bid + ask ("mid2") so half-ticks stay
integers. Markouts compare the real midpoint just before the fill's trigger
with the midpoint 100 ms, 1 s and 10 s later.

## Python

`python/src/bindings.cpp` exposes the config, the result types, `Context`,
and a subclassable `Strategy`. The simulation runs in C++ and calls Python
only for the strategy's own events. `tickforge.strategies.JoinBestQuoterPy`
is a Python copy of the C++ demo strategy, and a test requires identical
decisions and fills on COLB.
