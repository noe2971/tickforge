#include "tickforge/book/top_levels.hpp"

#include <algorithm>
#include <memory>
#include <numeric>
#include <stdexcept>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/itch_replay.hpp"
#include "tickforge/book/reference_book.hpp"
#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/endian.hpp"
#include "tickforge/itch/stream.hpp"

namespace tickforge::book {

namespace {

std::unique_ptr<io::ByteSource> open_input(const std::string& path) {
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".tfc") == 0)
        return std::make_unique<itch::CompactReader>(path);
    return io::open_byte_source(path);
}

bool is_book_message(char t) {
    return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

template <typename Book>
std::vector<TopLevels> run(const std::string& path, std::string_view symbol, const std::vector<std::uint64_t>& times,
                           std::size_t levels) {
    // Visit the requested times in increasing order, remember where each goes.
    std::vector<std::size_t> order(times.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return times[a] < times[b]; });

    std::vector<TopLevels> out(times.size());
    ItchReplay<Book> replay;
    std::uint16_t locate = 0;
    bool found = false;
    std::uint64_t applied = 0;
    std::uint64_t last_ts = 0;
    std::size_t next = 0;
    std::vector<LevelView> scratch(levels);

    auto snapshot = [&](std::size_t idx) {
        TopLevels& t = out[idx];
        t.before_ns = times[idx];
        t.messages_applied = applied;
        t.last_applied_ns = last_ts;
        if (const Book* b = found ? replay.books().find(locate) : nullptr) {
            scratch.resize(levels);
            t.bids.assign(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(b->depth(Side::buy, scratch.data(), levels)));
            t.asks.assign(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(b->depth(Side::sell, scratch.data(), levels)));
        }
    };

    auto src = open_input(path);
    itch::for_each_frame(*src, [&](const std::uint8_t* msg, std::size_t len) -> bool {
        if (len < itch::kHeaderSize) return true;
        const char type = static_cast<char>(msg[0]);
        if (!found) {
            if (type == itch::StockDirectory::kType && len == itch::StockDirectory::kSize) {
                const auto d = itch::decode<itch::StockDirectory>(msg);
                if (d.stock.view() == symbol) {
                    locate = d.header.stock_locate;
                    found = true;
                }
            }
            if (!found) return true;
        }
        if (itch::load_be16(msg + 1) != locate) return true;
        const std::uint64_t ts = itch::load_be48(msg + 5);
        while (next < order.size() && ts >= times[order[next]]) snapshot(order[next++]);
        if (next == order.size()) return false;  // every requested time is done
        itch::dispatch(msg, len, replay);
        if (is_book_message(type)) {
            ++applied;
            last_ts = ts;
        }
        return true;
    });
    if (!found) throw std::runtime_error("symbol " + std::string(symbol) + " is not in the stock directory of " + path);
    while (next < order.size()) snapshot(order[next++]);  // times after the last message
    return out;
}

}  // namespace

std::vector<TopLevels> top_levels_at(const std::string& path, std::string_view symbol,
                                     const std::vector<std::uint64_t>& times_ns, std::size_t levels, Engine engine) {
    if (engine == Engine::reference) return run<ReferenceBook>(path, symbol, times_ns, levels);
    return run<FastBook>(path, symbol, times_ns, levels);
}

std::optional<std::uint64_t> parse_time_of_day(std::string_view t) {
    auto digits = [](std::string_view s, std::uint64_t& v) {
        if (s.empty()) return false;
        v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return true;
    };
    if (t.size() < 8 || t[2] != ':' || t[5] != ':') return std::nullopt;
    std::uint64_t h, m, s, frac = 0;
    if (!digits(t.substr(0, 2), h) || !digits(t.substr(3, 2), m) || !digits(t.substr(6, 2), s)) return std::nullopt;
    if (h > 23 || m > 59 || s > 59) return std::nullopt;
    if (t.size() > 8) {
        if (t[8] != '.' || t.size() > 18) return std::nullopt;
        const std::string_view f = t.substr(9);
        if (!digits(f, frac)) return std::nullopt;
        for (std::size_t i = f.size(); i < 9; ++i) frac *= 10;
    }
    return ((h * 60 + m) * 60 + s) * 1'000'000'000ULL + frac;
}

}  // namespace tickforge::book
