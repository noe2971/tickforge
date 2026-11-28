#pragma once
// The strategy interface (C++; Python strategies subclass the same class
// through pybind11, see python/src/bindings.cpp).
//
// A strategy only ever sees the world through Context:
//   * its own copy of the book, built from messages *delivered* to it (after
//     the market-data latency), never the exchange-side book;
//   * its own orders and position, as reported back to it;
//   * its clock, now() = the time of the decision being made.
// Every action it takes reaches the exchange order_latency_ns later.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/sim/types.hpp"

namespace tickforge::sim {

class Context {
public:
    virtual ~Context() = default;

    // Strategy time of the current decision (ns after midnight ET).
    virtual std::uint64_t now() const = 0;

    // The strategy's view of the real book (delivered messages only).
    virtual const book::FastBook& book() const = 0;
    std::optional<book::LevelView> best_bid() const { return book().best(Side::buy); }
    std::optional<book::LevelView> best_ask() const { return book().best(Side::sell); }
    std::vector<book::LevelView> depth(Side side, std::size_t n) const {
        std::vector<book::LevelView> out(n);
        out.resize(book().depth(side, out.data(), n));
        return out;
    }
    // Latest Stock Trading Action state delivered ('T' trading, 'H', 'P', 'Q'; 0 = none yet).
    virtual char trading_state() const = 0;

    // Sends a new order; returns its id (the order is pending until it arrives).
    virtual ShadowId place(Side side, Price price, Qty qty, OrderFlags flags = {}) = 0;
    // Cancels `shares` of an order (0 = all of it). A partial cancel keeps priority.
    virtual void cancel(ShadowId id, Qty shares = 0) = 0;
    // Replaces an order with a new one (new price and size, new id, back of the queue).
    virtual ShadowId replace(ShadowId id, Price price, Qty qty) = 0;
    // Asks for on_timer at strategy time `at_ns` (now() if in the past); returns the timer id.
    virtual std::uint64_t set_timer(std::uint64_t at_ns) = 0;

    // As reported to the strategy so far.
    virtual std::int64_t position() const = 0;
    virtual Micros cash() const = 0;
    virtual const KnownOrder* order(ShadowId id) const = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_start(Context&) {}
    virtual void on_market(Context&, const MarketEvent&) {}
    virtual void on_order(Context&, const OrderUpdate&) {}
    virtual void on_fill(Context&, const FillReport&) {}
    virtual void on_timer(Context&, std::uint64_t /*timer_id*/) {}
    // Called once at the end time, after every shadow order was cancelled.
    // Actions sent from here are rejected (after_end).
    virtual void on_end(Context&) {}
};

}  // namespace tickforge::sim
