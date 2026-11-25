#include "tickforge/itch/compact_file.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "tickforge/itch/endian.hpp"

namespace tickforge::itch {

namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

void read_exact(io::FileSource& f, std::uint8_t* dst, std::size_t n, const char* what) {
    std::size_t done = 0;
    while (done < n) {
        const std::size_t got = f.read(dst + done, n - done);
        if (got == 0) throw std::runtime_error(std::string("compact file truncated in ") + what);
        done += got;
    }
}

}  // namespace

// ---- Writer -------------------------------------------------------------------

CompactWriter::CompactWriter(const std::string& path, std::string source, const std::vector<std::string>& symbols)
    : path_(path) {
    if (symbols.size() > 0xFFFF) throw std::invalid_argument("too many symbols");
    if (source.size() > 0xFFFF) throw std::invalid_argument("source name too long");
    header_.source = std::move(source);
    for (const auto& s : symbols) header_.symbols.push_back({0, make_symbol(s)});
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) throw std::runtime_error("cannot create " + path);
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    write_header();  // placeholder; rewritten with final values in close()
}

CompactWriter::~CompactWriter() {
    if (file_ != nullptr) {
        try {
            close();
        } catch (...) {
            // Destructors must not throw; call close() explicitly to see errors.
        }
    }
}

void CompactWriter::set_locate(const std::string& symbol, std::uint16_t locate) {
    const Symbol key = make_symbol(symbol);
    for (auto& s : header_.symbols)
        if (s.symbol == key) s.locate = locate;
}

void CompactWriter::write(const std::uint8_t* msg, std::size_t len) {
    if (len > 0xFFFF) throw std::invalid_argument("ITCH message longer than 65535 bytes");
    std::uint8_t prefix[2];
    store_be16(prefix, static_cast<std::uint16_t>(len));
    if (std::fwrite(prefix, 1, 2, file_) != 2 || std::fwrite(msg, 1, len, file_) != len)
        throw std::runtime_error("write failed on " + path_);
    ++header_.message_count;
    header_.payload_bytes += 2 + len;
}

void CompactWriter::write_header() {
    std::vector<std::uint8_t> h(std::begin(kCompactMagic), std::end(kCompactMagic));
    put_u16(h, header_.version);
    put_u16(h, static_cast<std::uint16_t>(header_.symbols.size()));
    put_u64(h, header_.message_count);
    put_u64(h, header_.payload_bytes);
    put_u16(h, static_cast<std::uint16_t>(header_.source.size()));
    h.insert(h.end(), header_.source.begin(), header_.source.end());
    for (const auto& s : header_.symbols) {
        put_u16(h, s.locate);
        h.insert(h.end(), s.symbol.chars.begin(), s.symbol.chars.end());
    }
    if (std::fwrite(h.data(), 1, h.size(), file_) != h.size())
        throw std::runtime_error("header write failed on " + path_);
}

void CompactWriter::close() {
    if (file_ == nullptr) return;
    // The header has a fixed size for a given symbol list, so rewriting it in
    // place can't overwrite payload bytes.
    if (std::fseek(file_, 0, SEEK_SET) != 0) throw std::runtime_error("seek failed on " + path_);
    write_header();
    const bool ok = std::fflush(file_) == 0;
    std::fclose(file_);
    file_ = nullptr;
    if (!ok) throw std::runtime_error("flush failed on " + path_);
}

// ---- Reader -------------------------------------------------------------------

CompactReader::CompactReader(const std::string& path) : file_(path) {
    std::uint8_t fixed[30];
    read_exact(file_, fixed, sizeof fixed, "header");
    if (std::memcmp(fixed, kCompactMagic, 8) != 0) throw std::runtime_error(path + " is not a Tickforge compact ITCH file");
    header_.version = load_be16(fixed + 8);
    if (header_.version != kCompactVersion)
        throw std::runtime_error(path + ": unsupported compact format version " + std::to_string(header_.version));
    const std::uint16_t n_symbols = load_be16(fixed + 10);
    header_.message_count = load_be64(fixed + 12);
    header_.payload_bytes = load_be64(fixed + 20);
    const std::uint16_t source_len = load_be16(fixed + 28);

    header_.source.resize(source_len);
    read_exact(file_, reinterpret_cast<std::uint8_t*>(header_.source.data()), source_len, "source name");

    std::vector<std::uint8_t> table(10u * n_symbols);
    read_exact(file_, table.data(), table.size(), "symbol table");
    for (std::size_t i = 0; i < n_symbols; ++i) {
        CompactSymbol s;
        s.locate = load_be16(table.data() + 10 * i);
        std::memcpy(s.symbol.chars.data(), table.data() + 10 * i + 2, 8);
        header_.symbols.push_back(s);
    }

    const std::uint64_t header_bytes = 30 + source_len + table.size();
    if (file_.size() != header_bytes + header_.payload_bytes)
        throw std::runtime_error(path + ": file size does not match header (truncated or corrupt)");
    remaining_ = header_.payload_bytes;
}

std::size_t CompactReader::read(std::uint8_t* dst, std::size_t n) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(n, remaining_));
    if (want == 0) return 0;
    const std::size_t got = file_.read(dst, want);
    if (got == 0) throw std::runtime_error("compact file ended early");
    remaining_ -= got;
    return got;
}

}  // namespace tickforge::itch
