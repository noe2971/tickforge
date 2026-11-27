#pragma once
// Replaying ITCH 5.0 into order books: one book per stock locate.
//
// ItchReplay is an ITCH handler (see itch/decode.hpp) that turns each
// book-changing message into a replay-mode call on the right book:
//
//   A / F  Add Order            -> add(ref, side, price, shares)
//   E      Order Executed       -> execute(ref, shares)
//   C      Executed With Price  -> execute(ref, shares)   (book effect is the same)
//   X      Order Cancel         -> cancel(ref, shares)
//   D      Order Delete         -> remove(ref)
//   U      Order Replace        -> replace(old ref, new ref, price, shares)
//
// Everything else (trades of hidden orders, crosses, NOII, ...) does not
// change the visible book and is ignored here; ReplayValidator accounts for it.
//
// The Probe template parameter lets the latency benchmark wrap each book call
// with a timer while the throughput benchmark uses NoProbe, which compiles to
// nothing, so both measure the same code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "tickforge/book/types.hpp"
#include "tickforge/itch/decode.hpp"

namespace tickforge::book {

// Which kind of book call a message became (latency is reported per kind).
enum class OpKind : std::uint8_t { add = 0, execute, cancel, remove, replace };
inline constexpr std::size_t kOpKinds = 5;
constexpr const char* to_string(OpKind k) noexcept {
    switch (k) {
        case OpKind::add: return "add (A+F)";
        case OpKind::execute: return "execute (E+C)";
        case OpKind::cancel: return "cancel (X)";
        case OpKind::remove: return "delete (D)";
        case OpKind::replace: return "replace (U)";
    }
    return "?";
}

constexpr Side side_from_itch(char c) noexcept { return c == 'B' ? Side::buy : Side::sell; }

// One book per stock locate (ITCH's per-day integer id for a security).
template <typename Book>
class BookSet {
public:
    BookSet() : books_(65536) {}

    Book& get(std::uint16_t locate) {
        auto& b = books_[locate];
        if (!b) b = std::make_unique<Book>();
        return *b;
    }
    Book* find(std::uint16_t locate) const noexcept { return books_[locate].get(); }

    // f(locate, const Book&) for every book that exists, in locate order.
    template <typename F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < books_.size(); ++i)
            if (books_[i]) f(static_cast<std::uint16_t>(i), *books_[i]);
    }

private:
    std::vector<std::unique_ptr<Book>> books_;
};

// Calls the book operation directly.
struct NoProbe {
    template <typename F>
    decltype(auto) operator()(OpKind, F&& op) {
        return op();
    }
};

template <typename Book, typename Probe = NoProbe>
class ItchReplay : public itch::NullHandler {
public:
    using itch::NullHandler::on;

    ItchReplay() = default;
    explicit ItchReplay(Probe probe) : probe_(std::move(probe)) {}

    void on(const itch::AddOrder& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::add, probe_(OpKind::add, [&] {
            return b.add(m.order_reference_number, side_from_itch(m.side), m.price, m.shares);
        }));
    }
    void on(const itch::AddOrderMpid& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::add, probe_(OpKind::add, [&] {
            return b.add(m.order_reference_number, side_from_itch(m.side), m.price, m.shares);
        }));
    }
    void on(const itch::OrderExecuted& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::execute, probe_(OpKind::execute, [&] {
            return b.execute(m.order_reference_number, m.executed_shares).status;
        }));
    }
    void on(const itch::OrderExecutedWithPrice& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::execute, probe_(OpKind::execute, [&] {
            return b.execute(m.order_reference_number, m.executed_shares).status;
        }));
    }
    void on(const itch::OrderCancel& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::cancel, probe_(OpKind::cancel, [&] {
            return b.cancel(m.order_reference_number, m.cancelled_shares);
        }));
    }
    void on(const itch::OrderDelete& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::remove, probe_(OpKind::remove, [&] { return b.remove(m.order_reference_number); }));
    }
    void on(const itch::OrderReplace& m) {
        Book& b = books_.get(m.header.stock_locate);
        note(OpKind::replace, probe_(OpKind::replace, [&] {
            return b.replace(m.original_order_reference_number, m.new_order_reference_number, m.price, m.shares);
        }));
    }

    BookSet<Book>& books() noexcept { return books_; }
    const BookSet<Book>& books() const noexcept { return books_; }
    Probe& probe() noexcept { return probe_; }

    // results[kind][status]: how many calls of each kind returned each status.
    const std::array<std::array<std::uint64_t, kStatusCount>, kOpKinds>& results() const noexcept { return results_; }
    std::uint64_t book_messages() const noexcept { return book_messages_; }

private:
    void note(OpKind k, Status s) noexcept {
        ++results_[static_cast<std::size_t>(k)][static_cast<std::size_t>(s)];
        ++book_messages_;
    }

    BookSet<Book> books_;
    Probe probe_{};
    std::array<std::array<std::uint64_t, kStatusCount>, kOpKinds> results_{};
    std::uint64_t book_messages_ = 0;
};

// Order-sensitive digest of every order in every book (side, level price,
// order id, shares, in priority order). Two engines that built the same
// books produce the same digest.
template <typename Book>
std::uint64_t book_digest(const BookSet<Book>& books) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; };
    books.for_each([&](std::uint16_t locate, const Book& b) {
        for (Side s : {Side::buy, Side::sell}) {
            b.for_each_order(s, [&](Price p, OrderId id, Qty q) {
                mix(locate);
                mix(static_cast<std::uint64_t>(s));
                mix(static_cast<std::uint64_t>(p));
                mix(id);
                mix(q);
            });
        }
    });
    return h;
}

}  // namespace tickforge::book
