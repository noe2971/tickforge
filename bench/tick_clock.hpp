#pragma once
// The cheapest monotonic clock available, for timing single book calls.
//
// On macOS this is mach_absolute_time(): one read of the CPU's system
// counter. On Apple silicon that counter runs at 24 MHz, so one tick is
// 41.667 ns and every latency is measured in whole ticks. Elsewhere it falls
// back to std::chrono::steady_clock in nanoseconds.

#include <chrono>
#include <cstdint>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace tickforge::bench {

inline std::uint64_t ticks() noexcept {
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Nanoseconds per tick.
inline double ns_per_tick() noexcept {
#if defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
#else
    return 1.0;
#endif
}

inline const char* clock_name() noexcept {
#if defined(__APPLE__)
    return "mach_absolute_time";
#else
    return "std::chrono::steady_clock";
#endif
}

}  // namespace tickforge::bench
