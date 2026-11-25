#pragma once
// Big-endian ("network byte order") loads used by the ITCH decoder.
//
// ITCH sends every integer most-significant byte first. x86 and ARM CPUs store
// integers least-significant byte first, so each multi-byte field has to be
// byte-swapped after loading. memcpy avoids unaligned-access undefined
// behaviour (ITCH fields sit at odd offsets); compilers turn it plus the
// bswap into a single load + `rev`/`bswap` instruction.

#include <bit>
#include <cstdint>
#include <cstring>

namespace tickforge::itch {

namespace detail {
template <typename T>
inline T byteswap_if_little(T v) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(v));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(v));
    } else {
        static_assert(sizeof(T) == 8);
        return static_cast<T>(__builtin_bswap64(v));
    }
}
}  // namespace detail

inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return detail::byteswap_if_little(v);
}

inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return detail::byteswap_if_little(v);
}

// ITCH timestamps are 6-byte (48-bit) nanoseconds since midnight.
inline std::uint64_t load_be48(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(load_be16(p)) << 32) | load_be32(p + 2);
}

inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return detail::byteswap_if_little(v);
}

inline void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    v = detail::byteswap_if_little(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    v = detail::byteswap_if_little(v);
    std::memcpy(p, &v, sizeof v);
}

}  // namespace tickforge::itch
