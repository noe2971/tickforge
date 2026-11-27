#pragma once
// Exporting the top N price levels of one stock at any moment of the day.
//
// "The book at time T" means the book as it stood *just before* T: every
// message for that stock stamped strictly earlier than T is applied, and
// nothing stamped at or after T is. That is the same rule the simulator must
// follow (CLAUDE.md rule 2): something acting at time T may only see data
// from before T, including nothing from the same nanosecond.
//
// The replay stops at the first message for that stock stamped at or after
// the last requested time, so this relies on ITCH timestamps not going
// backwards (Rule D check 6 measures that on the real day).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tickforge/book/types.hpp"

namespace tickforge::book {

struct TopLevels {
    std::uint64_t before_ns = 0;      // the book just before this time (ns since midnight)
    std::vector<LevelView> bids;      // best first, at most N
    std::vector<LevelView> asks;      // best first, at most N
    std::uint64_t messages_applied = 0;  // book messages for the stock with timestamp < before_ns
    std::uint64_t last_applied_ns = 0;   // timestamp of the last one (0 if none)
    friend bool operator==(const TopLevels&, const TopLevels&) = default;
};

enum class Engine { fast, reference };

// One pass over `path` (.gz, raw ITCH, or .tfc compact file); returns one
// result per requested time, in the order given. Throws if the symbol never
// appears in the file's stock directory.
std::vector<TopLevels> top_levels_at(const std::string& path, std::string_view symbol,
                                     const std::vector<std::uint64_t>& times_ns, std::size_t levels = 10,
                                     Engine engine = Engine::fast);

inline TopLevels top_levels_at(const std::string& path, std::string_view symbol, std::uint64_t time_ns,
                               std::size_t levels = 10, Engine engine = Engine::fast) {
    return top_levels_at(path, symbol, std::vector<std::uint64_t>{time_ns}, levels, engine).front();
}

// "HH:MM:SS" or "HH:MM:SS.fraction" (up to 9 digits) -> ns since midnight.
std::optional<std::uint64_t> parse_time_of_day(std::string_view text);

}  // namespace tickforge::book
