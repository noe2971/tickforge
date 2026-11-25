#pragma once
// "Compact" per-symbol ITCH file: just the messages for a few symbols, stored
// uncompressed so later steps can replay them in milliseconds instead of
// decompressing the whole 4.8 GB day.
//
// Layout (all integers big-endian, like ITCH itself):
//
//   offset  size  field
//   0       8     magic "TFITCH5C"
//   8       2     format version (currently 1)
//   10      2     symbol count N
//   12      8     message count
//   20      8     payload size in bytes
//   28      2     source name length S
//   30      S     source name (e.g. "01302019.NASDAQ_ITCH50.gz")
//   30+S    10*N  N x { 2-byte stock locate, 8-byte space-padded symbol }
//   ...           payload: messages in original order, each framed exactly as
//                 in the Nasdaq binary file: [2-byte length][message bytes]
//
// The payload keeps the original ITCH bytes untouched, so it is decoded by
// the same tested decoder and nothing is lost or re-encoded. A locate of 0 in
// the symbol table means that symbol was requested but not found that day.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/messages.hpp"

namespace tickforge::itch {

inline constexpr char kCompactMagic[8] = {'T', 'F', 'I', 'T', 'C', 'H', '5', 'C'};
inline constexpr std::uint16_t kCompactVersion = 1;

struct CompactSymbol {
    std::uint16_t locate = 0;
    Symbol symbol;
    friend bool operator==(const CompactSymbol&, const CompactSymbol&) = default;
};

struct CompactHeader {
    std::uint16_t version = kCompactVersion;
    std::string source;
    std::vector<CompactSymbol> symbols;
    std::uint64_t message_count = 0;
    std::uint64_t payload_bytes = 0;
};

class CompactWriter {
public:
    // Creates/truncates path. Locates may be filled in later with set_locate()
    // because they're only known once the day's 'R' messages have been read.
    CompactWriter(const std::string& path, std::string source, const std::vector<std::string>& symbols);
    ~CompactWriter();
    CompactWriter(const CompactWriter&) = delete;
    CompactWriter& operator=(const CompactWriter&) = delete;

    void set_locate(const std::string& symbol, std::uint16_t locate);
    void write(const std::uint8_t* msg, std::size_t len);
    // Rewrites the header with final counts/locates and closes the file.
    void close();

    const CompactHeader& header() const noexcept { return header_; }

private:
    void write_header();
    std::FILE* file_ = nullptr;
    std::string path_;
    CompactHeader header_;
};

// Reads a compact file: parses the header, then acts as a ByteSource over the
// framed payload, so it can be fed straight into for_each_frame/parse_stream.
class CompactReader final : public io::ByteSource {
public:
    explicit CompactReader(const std::string& path);
    const CompactHeader& header() const noexcept { return header_; }
    std::size_t read(std::uint8_t* dst, std::size_t n) override;

private:
    io::FileSource file_;
    CompactHeader header_;
    std::uint64_t remaining_ = 0;
};

}  // namespace tickforge::itch
