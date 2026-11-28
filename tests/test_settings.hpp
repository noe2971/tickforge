#pragma once
// How much of the long randomized tests to run.
//
// By default (locally, and in scripts/check.sh, which gates every commit)
// they run in full. CI sets TICKFORGE_SHORT_TESTS=1: they then run a fixed,
// smaller set of the same seeds with exactly the same checks, so a push gets
// feedback in minutes. Nothing is skipped and no check is relaxed; the
// setting used is printed in the test output.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace tickforge::test {

inline bool short_tests() {
    const char* v = std::getenv("TICKFORGE_SHORT_TESTS");
    return v != nullptr && std::string_view(v) != "" && std::string_view(v) != "0";
}

inline std::uint64_t full_or_short(std::uint64_t full, std::uint64_t short_count, const char* what) {
    const bool s = short_tests();
    std::cout << "[test setting] " << what << ": "
              << (s ? std::to_string(short_count) + " (short CI setting; the full run is " + std::to_string(full) + ")"
                    : std::to_string(full) + " (full run)")
              << "\n";
    return s ? short_count : full;
}

}  // namespace tickforge::test
