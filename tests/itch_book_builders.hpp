#pragma once
// Hand-built ITCH messages for the order-book replay tests. Field layouts
// follow the ITCH 5.0 spec and are written with the independent Bytes
// builder, not the production decoder.

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "itch_test_util.hpp"

namespace tickforge::test::itchmsg {

inline Bytes system_event(std::uint64_t ts, char code) { return Bytes().header('S', 0, 0, ts).ch(code); }

inline Bytes directory(std::uint16_t loc, std::uint64_t ts, const std::string& sym) {
    return Bytes()
        .header('R', loc, 0, ts)
        .alpha(sym, 8)
        .ch('Q').ch('N').u32(100).ch('N').ch('C').alpha("Z", 2).ch('P').ch('N').ch('N').ch('1').ch('N')
        .u32(0).ch('N');
}

inline Bytes trading_action(std::uint16_t loc, std::uint64_t ts, const std::string& sym, char state) {
    return Bytes().header('H', loc, 0, ts).alpha(sym, 8).ch(state).ch(' ').alpha("", 4);
}

inline Bytes add(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t shares,
                 const std::string& sym, std::uint32_t price) {
    return Bytes().header('A', loc, 0, ts).u64(ref).ch(side).u32(shares).alpha(sym, 8).u32(price);
}

inline Bytes add_mpid(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t shares,
                      const std::string& sym, std::uint32_t price) {
    return Bytes().header('F', loc, 0, ts).u64(ref).ch(side).u32(shares).alpha(sym, 8).u32(price).alpha("MPID", 4);
}

inline Bytes executed(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    return Bytes().header('E', loc, 0, ts).u64(ref).u32(shares).u64(1);
}

inline Bytes executed_price(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                            char printable, std::uint32_t price) {
    return Bytes().header('C', loc, 0, ts).u64(ref).u32(shares).u64(2).ch(printable).u32(price);
}

inline Bytes cancel(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    return Bytes().header('X', loc, 0, ts).u64(ref).u32(shares);
}

inline Bytes del(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref) {
    return Bytes().header('D', loc, 0, ts).u64(ref);
}

inline Bytes replace(std::uint16_t loc, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
                     std::uint32_t shares, std::uint32_t price) {
    return Bytes().header('U', loc, 0, ts).u64(old_ref).u64(new_ref).u32(shares).u32(price);
}

inline Bytes trade(std::uint16_t loc, std::uint64_t ts, char side, std::uint32_t shares, const std::string& sym,
                   std::uint32_t price) {
    return Bytes().header('P', loc, 0, ts).u64(0).ch(side).u32(shares).alpha(sym, 8).u32(price).u64(3);
}

inline Bytes cross(std::uint16_t loc, std::uint64_t ts, std::uint64_t shares, const std::string& sym,
                   std::uint32_t price, char type) {
    return Bytes().header('Q', loc, 0, ts).u64(shares).alpha(sym, 8).u32(price).u64(4).ch(type);
}

// Feeds messages straight to a handler.
template <typename H>
void feed(H& h, const std::vector<Bytes>& msgs) {
    for (const auto& m : msgs) itch::dispatch(m.b.data(), m.b.size(), h);
}

// Writes messages as a raw (uncompressed) ITCH file.
inline void write_file(const std::string& path, const std::vector<Bytes>& msgs) {
    std::ofstream f(path, std::ios::binary);
    for (const auto& m : msgs) {
        const auto framed = m.framed();
        f.write(reinterpret_cast<const char*>(framed.data()), static_cast<std::streamsize>(framed.size()));
    }
}

}  // namespace tickforge::test::itchmsg
