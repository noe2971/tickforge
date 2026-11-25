// itch_throughput: full-day ITCH 5.0 parse-throughput benchmark.
//
//   itch_throughput <file.gz> [measured_passes=3] [warmup_passes=1]
//
// Method (locked in PROGRESS.md, "Rule B", before it was first run):
// each pass streams the gzip file with zlib, frames every message on its
// 2-byte length prefix, decodes it into its typed struct and folds decoded
// fields into a checksum. Wall time is std::chrono::steady_clock; the framer
// also splits time into "read" (decompression + file I/O) and "parse"
// (framing + decode + handler). MB = 10^6 bytes.
//
// Prints one markdown table row per measured pass, then the median row.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>

#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;
using namespace tickforge::itch;

namespace {

// Touches the decoded fields so the decode work can't be optimised away.
struct ChecksumHandler : NullHandler {
    using NullHandler::on;
    std::uint64_t sum = 0;

    void mix(std::uint64_t v) noexcept { sum = (sum ^ v) * 0x100000001b3ULL; }
    void mix(const Header& h) noexcept { mix(h.timestamp_ns ^ (std::uint64_t{h.stock_locate} << 48) ^ h.tracking_number); }

    template <typename M>
    void on(const M& m) noexcept { mix(m.header); }

    void on(const AddOrder& m) noexcept {
        mix(m.header); mix(m.order_reference_number); mix(m.shares); mix(m.price);
        mix(static_cast<std::uint64_t>(m.side)); mix(static_cast<std::uint64_t>(m.stock.chars[0]));
    }
    void on(const AddOrderMpid& m) noexcept {
        mix(m.header); mix(m.order_reference_number); mix(m.shares); mix(m.price);
        mix(static_cast<std::uint64_t>(m.side)); mix(static_cast<std::uint64_t>(m.attribution.chars[0]));
    }
    void on(const OrderExecuted& m) noexcept {
        mix(m.header); mix(m.order_reference_number); mix(m.executed_shares); mix(m.match_number);
    }
    void on(const OrderExecutedWithPrice& m) noexcept {
        mix(m.header); mix(m.order_reference_number); mix(m.executed_shares); mix(m.match_number);
        mix(m.execution_price); mix(static_cast<std::uint64_t>(m.printable));
    }
    void on(const OrderCancel& m) noexcept { mix(m.header); mix(m.order_reference_number); mix(m.cancelled_shares); }
    void on(const OrderDelete& m) noexcept { mix(m.header); mix(m.order_reference_number); }
    void on(const OrderReplace& m) noexcept {
        mix(m.header); mix(m.original_order_reference_number); mix(m.new_order_reference_number);
        mix(m.shares); mix(m.price);
    }
    void on(const Trade& m) noexcept {
        mix(m.header); mix(m.order_reference_number); mix(m.shares); mix(m.price); mix(m.match_number);
    }
    void on(const CrossTrade& m) noexcept {
        mix(m.header); mix(m.shares); mix(m.cross_price); mix(m.match_number); mix(static_cast<std::uint64_t>(m.cross_type));
    }
};

struct Pass {
    double wall_s;
    std::uint64_t messages;
    std::uint64_t bytes;
    std::uint64_t compressed;
    double read_s;
    double parse_s;
    std::uint64_t checksum;
};

Pass run_pass(const std::string& path) {
    const auto t0 = std::chrono::steady_clock::now();
    io::GzipFileSource src(path);
    ChecksumHandler h;
    const StreamStats st = parse_stream(src, h);
    const auto t1 = std::chrono::steady_clock::now();
    if (st.unknown_type != 0 || st.bad_length != 0) throw std::runtime_error("decode errors during benchmark pass");
    return {std::chrono::duration<double>(t1 - t0).count(), st.messages, st.bytes, src.compressed_bytes_read(),
            static_cast<double>(st.read_ns) / 1e9, static_cast<double>(st.parse_ns) / 1e9, h.sum};
}

void print_row(const std::string& label, const Pass& p) {
    std::printf("| %s | %.2f | %.2f | %.1f | %.1f | %.2f | %.2f | %.1f | %.1f |\n", label.c_str(), p.wall_s,
                static_cast<double>(p.messages) / p.wall_s / 1e6, static_cast<double>(p.bytes) / p.wall_s / 1e6,
                static_cast<double>(p.compressed) / p.wall_s / 1e6, p.read_s, p.parse_s,
                static_cast<double>(p.messages) / p.parse_s / 1e6, static_cast<double>(p.bytes) / p.parse_s / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: itch_throughput <file.gz> [measured_passes=3] [warmup_passes=1]\n";
        return 2;
    }
    const std::string path = argv[1];
    const int passes = argc > 2 ? std::stoi(argv[2]) : 3;
    const int warmups = argc > 3 ? std::stoi(argv[3]) : 1;

    try {
        for (int i = 0; i < warmups; ++i) {
            const Pass w = run_pass(path);
            std::fprintf(stderr, "warm-up pass %d: %.2f s (discarded)\n", i + 1, w.wall_s);
        }
        std::vector<Pass> results;
        for (int i = 0; i < passes; ++i) {
            results.push_back(run_pass(path));
            std::fprintf(stderr, "pass %d: %.2f s\n", i + 1, results.back().wall_s);
            if (results.back().checksum != results.front().checksum) throw std::runtime_error("checksum differs between passes");
        }

        const Pass& f = results.front();
        std::printf("zlib %s; %llu messages; %llu uncompressed bytes; %llu compressed bytes; checksum %016llx\n\n",
                    zlibVersion(), static_cast<unsigned long long>(f.messages), static_cast<unsigned long long>(f.bytes),
                    static_cast<unsigned long long>(f.compressed), static_cast<unsigned long long>(f.checksum));
        std::printf("| Pass | Wall s | M msg/s | Uncompressed MB/s | Compressed MB/s | Read+inflate s | Frame+decode s "
                    "| Decode-only M msg/s | Decode-only MB/s |\n");
        std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
        for (std::size_t i = 0; i < results.size(); ++i) print_row(std::to_string(i + 1), results[i]);

        std::vector<Pass> sorted = results;
        std::sort(sorted.begin(), sorted.end(), [](const Pass& a, const Pass& b) { return a.wall_s < b.wall_s; });
        print_row("**median**", sorted[sorted.size() / 2]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "itch_throughput: " << e.what() << "\n";
        return 1;
    }
}
