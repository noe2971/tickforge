#pragma once
// Helpers for building ITCH byte strings by hand in tests.
//
// The builder writes big-endian integers with explicit shifts, independently
// of the production decoder's memcpy+bswap code, so a bug in one is not
// silently mirrored in the other.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <unistd.h>
#include <vector>

#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/decode.hpp"

namespace tickforge::test {

struct Bytes {
    std::vector<std::uint8_t> b;

    Bytes& u8(std::uint8_t v) {
        b.push_back(v);
        return *this;
    }
    Bytes& ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
    Bytes& u16(std::uint16_t v) { return be(v, 2); }
    Bytes& u32(std::uint32_t v) { return be(v, 4); }
    Bytes& u48(std::uint64_t v) { return be(v, 6); }
    Bytes& u64(std::uint64_t v) { return be(v, 8); }
    // Left-justified, right-padded with spaces to width n (spec "Alpha").
    Bytes& alpha(std::string_view s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) ch(i < s.size() ? s[i] : ' ');
        return *this;
    }
    // Common 11-byte header.
    Bytes& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        return ch(type).u16(locate).u16(tracking).u48(ts);
    }
    // The message framed as in the Nasdaq file: 2-byte length + bytes.
    std::vector<std::uint8_t> framed() const {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(b.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }

private:
    Bytes& be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        return *this;
    }
};

using AnyMessage =
    std::variant<std::monostate, itch::SystemEvent, itch::StockDirectory, itch::StockTradingAction,
                 itch::RegShoRestriction, itch::MarketParticipantPosition, itch::MwcbDeclineLevel, itch::MwcbStatus,
                 itch::IpoQuotingPeriodUpdate, itch::LuldAuctionCollar, itch::OperationalHalt, itch::AddOrder,
                 itch::AddOrderMpid, itch::OrderExecuted, itch::OrderExecutedWithPrice, itch::OrderCancel,
                 itch::OrderDelete, itch::OrderReplace, itch::Trade, itch::CrossTrade, itch::BrokenTrade, itch::Noii,
                 itch::Rpii, itch::DlcrPriceDiscovery>;

// Records every decoded message.
struct Capture : itch::NullHandler {
    std::vector<AnyMessage> messages;
    int unknown = 0;
    int bad_length = 0;

    template <typename M>
    void on(const M& m) {
        messages.emplace_back(m);
    }
    void on_unknown(const std::uint8_t*, std::size_t) { ++unknown; }
    void on_bad_length(const std::uint8_t*, std::size_t) { ++bad_length; }
};

// A ByteSource over memory that returns at most `max_chunk` bytes per read, to
// force messages to straddle read boundaries.
class MemorySource final : public io::ByteSource {
public:
    MemorySource(std::vector<std::uint8_t> data, std::size_t max_chunk)
        : data_(std::move(data)), max_chunk_(max_chunk) {}
    std::size_t read(std::uint8_t* dst, std::size_t n) override {
        const std::size_t k = std::min({n, max_chunk_, data_.size() - pos_});
        std::memcpy(dst, data_.data() + pos_, k);
        pos_ += k;
        return k;
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t max_chunk_;
    std::size_t pos_ = 0;
};

// Unique path in the system temp directory; removes the file on destruction.
struct TempFile {
    std::string path;
    explicit TempFile(const std::string& name) {
        static std::atomic<int> counter{0};
        path = (std::filesystem::temp_directory_path() /
                ("tickforge_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + "_" + name))
                   .string();
        std::filesystem::remove(path);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

}  // namespace tickforge::test
