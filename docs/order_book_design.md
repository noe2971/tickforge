# Order book engine: design and why

Two engines implement the same interface (`include/tickforge/book/types.hpp`,
concept `OrderBook`):

- **`FastBook`** (`fast_book.hpp`) — used for replay, validation and benchmarks.
- **`ReferenceBook`** (`reference_book.hpp`) — `std::map` + `std::list`, written
  to be obviously correct. It is the answer key for the differential tests.

Both support two modes on the same book:

- **Replay mode** — `add`, `execute`, `cancel` (partial), `remove` (delete),
  `replace`. It applies what Nasdaq's ITCH feed says happened and never
  matches. An exchange feed already tells you about every trade, so the book
  must not invent trades of its own.
- **Matching mode** — `submit_limit`, `submit_market`. Strict price-time
  priority: best price first, and the oldest order first within a price.
  Fills are at the resting order's price. Partial fills leave the resting
  order at the front of its queue. A limit order's unfilled remainder rests
  on the book. A market order's remainder is cancelled.

Every call checks its inputs before changing anything, in the same order in
both engines: id (unknown / duplicate), then shares, then price. A call that
returns an error leaves the book exactly as it was.

## Prices are integers

A price is an `int64` count of $0.0001 ticks. That is exactly ITCH's
`Price(4)` field ($1.2345 is 12345), so no price is ever converted or rounded,
and two prices are equal only if they are the same integer. Floating point
would make "is this the same price level?" an approximate question.

## The data structures

```
 side array (bids)  — sorted worst → best, best at back()
 ┌──────────┬──────────┬──────────┬──────────┐
 │ 99.97    │ 99.98    │ 99.99    │ 100.00   │  ← best bid: back(), O(1)
 └────┬─────┴────┬─────┴────┬─────┴────┬─────┘
      │          │          │          ▼
      ▼          ▼          ▼     Level{price, shares, orders, head, tail}
                                       │head                    │tail
                                       ▼                        ▼
                                   Order ⇄ Order ⇄ Order ⇄ Order      (intrusive list,
                                   oldest                   newest      time priority)
 IdMap: order id ──► Order*      (each Order also points to its Level)
 ObjectPool<Order>, ObjectPool<Level>: where every Order and Level lives
```

### 1. Orders: an intrusive doubly linked list per price level

Each `Order` stores its own `prev`/`next` pointers and a pointer to its
`Level`. The list nodes are the orders themselves, which is what "intrusive"
means.

- Joining the back of the queue, or leaving it from anywhere (a cancel in the
  middle of a queue), is a few pointer writes: O(1), no search.
- No separate list-node allocation, unlike `std::list`.
- Because the order points to its level, `execute`, `cancel` and `delete` by
  id never search for the price. Id → order goes through the hash map, and
  order → level is one pointer.
- `head` is the oldest order, the one that trades first. Matching always takes
  `head`, so time priority is built into the structure.

### 2. Price levels: a sorted flat array per side, best price at the back

Each side is a `std::vector<{key, Level*}>` sorted from worst to best price
(`key` = price for bids, −price for asks, so ascending key means worse to
better on both sides).

- **Best bid/ask is `back()`**: O(1).
- **The best level emptying is `pop_back()`**: O(1). That is the common case
  when an aggressive order sweeps the touch.
- **Adds at the best price, or ones that improve it**, take a fast path with
  no search.
- **Any other price** is found by binary search over contiguous memory. On
  30 Jan 2019 the deepest side of any book had 3,143 levels (AMZN's bids,
  measured in `results/replay_validation_01302019.md`), so that is at most 12
  comparisons over about 50 KB.
- Inserting a new level in the middle shifts only the entries *better* than
  it. Most activity is near the touch, so that is usually a few entries.

Why not the alternatives:

- **A flat array indexed directly by price tick** (`levels[price - base]`)
  makes every lookup O(1). But it needs one slot per possible tick, and real
  books are spread out. On this day AAPL had orders from **$0.0001 to
  $199,999.99**, which is about 2 billion ticks of $0.0001. The same was true
  for MSFT and INTC, and COLB's ran from $0.0072 to $199,999.99. Those far
  prices are market makers' "stub quotes". A direct array would need a
  sliding window plus an overflow structure: more code, more cases, more ways
  to be wrong.
- **A hash map keyed by price alone** finds a level in O(1) but knows nothing
  about order. When the best level empties, finding the next best means
  scanning prices one tick at a time.
- **A balanced tree (`std::map`)** keeps order but allocates a node per level
  and chases a pointer at every step. That is what the reference engine uses,
  on purpose.

### 3. Order id → order: an open-addressing hash map (`id_map.hpp`)

`execute`, `cancel`, `delete` and `replace` arrive with only an order
reference number, so id lookup must be O(1).

- One flat array of `{key, Order*}` slots. A lookup hashes to a slot and
  walks forward until it finds the key or an empty slot ("linear probing").
  Neighbouring slots share cache lines, so a lookup usually costs one cache
  miss.
- Fibonacci hashing (`key × 2^64/φ`, keep the top bits) spreads ITCH's
  nearly sequential order references evenly.
- The table is at most half full, so probe runs stay short.
- Erase uses **backward-shift deletion**: later entries of the probe run move
  back into the hole. No "tombstones" pile up after the 158 million deletes
  of a day.
- `std::unordered_map` would allocate a heap node on every insert and chase
  a pointer on every lookup.

### 4. Memory pools: the hot path never allocates (`object_pool.hpp`)

Every `Order` and `Level` comes from an `ObjectPool`: big blocks owned by the
pool, plus a free list of released objects.

- `acquire()` pops the free list, or bumps a pointer inside the current block.
  `release()` pushes onto the free list. Each is a handful of instructions.
- `malloc` is slow and unpredictable *in the tail*. It takes locks, manages
  size classes and sometimes asks the OS for memory. A pool turns "allocate an
  order" into pointer arithmetic, and its tail is as short as its median.
- Released objects are reused, so the working set stays small and hot in
  cache.
- The pool only calls the allocator when it has no free object left, which
  only happens at a new high-water mark of live orders. `reserve(n)`
  pre-allocates so that never happens below n. The id map and level arrays
  have `reserve` too.
- Proof, as a test: `tests/test_book_alloc.cpp` replaces global `operator new`
  with a counter. 1,000,000 mixed operations (add, execute, cancel, delete,
  replace, matching) on a reserved `FastBook` make **0** heap allocations. The
  same workload on `ReferenceBook` allocates on every add.
- Under AddressSanitizer the pool *poisons* released objects. Using an order
  after it was released is caught even though its memory never went back to
  `malloc`.

### 5. One book per stock

ITCH order references are unique across the whole day, but every
execute/cancel/delete/replace message also carries the stock locate. So the
replay keeps one small, independent book per stock (`BookSet`, indexed by
locate). The validation run confirmed that no message on the day referred to
an order of a different stock (`ref_in_other_stock` = 0).

## The life of one order

Take a resting sell, then an incoming buy in matching mode.

1. `add(42, sell, 100.05, 300)` checks that id 42 is not live
   (`IdMap::find`), that there are shares and that the price is positive.
   `level_for` finds the 100.05 ask level (fast path if it is the best ask,
   otherwise binary search) or creates one from the level pool. An `Order`
   comes from the order pool and is linked at the **tail** of the level's
   queue. The level's share total and order count go up, and 42 → order goes
   into the id map. No allocation.
2. `submit_limit(77, buy, 100.05, 120, sink)` looks at the best ask
   (`asks.back()`): 100.05 ≤ 100.05, so it crosses. The level's `head` is the
   oldest order there. Say that is order 42. The trade is min(120, 300) = 120
   shares. `sink` receives `Fill{maker 42, taker 77, 100.05, 120}`. Order 42
   drops to 180 shares and *stays at the head*: a partial fill keeps time
   priority. The buy is filled, so nothing rests.
3. `submit_market(78, buy, 500, sink)` takes the head again: 180 shares,
   `Fill{42, 78, 100.05, 180}`. Order 42 now has 0 shares, so `unlink` takes
   it out of the list. The level is now empty, so it is `pop_back`'d from the
   ask array and returned to the level pool. Order 42 leaves the id map and
   goes back to the order pool. The loop moves on to the next best ask with
   the remaining 320 shares, or stops if the side is empty. A market order's
   remainder never rests.

In replay mode the same steps happen through `execute(42, 120)`: same
`reduce` path, no matching.

## How correctness is checked

1. **Behaviour tests** (`tests/test_book_engines.cpp`) run every hand-written
   expectation against *both* engines (typed tests).
2. **Differential fuzz** (`include/tickforge/book/differential.hpp`,
   `tools/book_fuzz`, PROGRESS.md Rule C) runs random streams of valid and
   invalid calls through both engines. After *every* event it compares
   statuses, returned values, the exact fill sequence and the complete book:
   every level and every order in queue order. To check that the fuzzer has
   teeth, three bugs were planted one at a time (LIFO queue, a forgotten
   level total, an emptied level left behind). Each was caught within 13
   events.
3. **Real-day validation** (`tools/itch_validate`, Rule D) checks, for the
   whole 30 Jan 2019 file:
   - every execution against the book;
   - no crossed books in continuous trading;
   - an independent share ledger that must reconcile.
4. **Real-day lockstep diff** (`tools/itch_engine_diff`) replays the whole day
   through both engines at once.

## Known limits

- No self-trade prevention and no order owners. Matching mode has only
  plain limit and market orders: no hidden, reserve, pegged or
  immediate-or-cancel types.
- Adding a level deep inside a large book shifts every better level in the
  array. The worst case is O(levels on that side). It is rare in practice
  because new levels cluster near the touch, but it is the structure's real
  cost.
- The top-N export relies on timestamps never going backwards; that was
  measured to hold on this day.
