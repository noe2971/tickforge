// book_replay: full-day order book benchmarks (PROGRESS.md, Rule E).
//
//   book_replay <day.gz> <book_peaks.csv> --engine fast|reference
//               --mode throughput|latency [--passes 3] [--warmup 1] [--chunk-mib 1024]
//
// Each pass:
//   1. creates one book per stock locate listed in the peaks CSV and pre-sizes
//      it to that stock's measured peak (live orders, levels per side);
//   2. repeatedly inflates `chunk` bytes of whole ITCH frames into memory
//      (timed as "inflate"), then frames, decodes and applies every message
//      in that buffer to the books (timed as "book");
//   3. hashes every book at each chunk boundary and at the end (outside the
//      timed segments), so both engines can be checked for identical books at
//      many points of the day.
// Heap allocations are counted only inside the timed book segments.
//
// throughput mode times whole segments; latency mode additionally times every
// single book call with the tick clock (see tick_clock.hpp) into per-kind
// histograms. Prints markdown tables to stdout.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tick_clock.hpp"
#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/itch_replay.hpp"
#include "tickforge/book/reference_book.hpp"
#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/endian.hpp"

// ---- Allocation counter -----------------------------------------------------------
// Counts calls to operator new while g_counting is true (only inside the timed
// book segments; the benchmark is single-threaded).
namespace {
bool g_counting = false;
std::uint64_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) ++g_allocations;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace tickforge;
using namespace tickforge::book;

namespace {

using Clock = std::chrono::steady_clock;

struct Peak {
    std::uint16_t locate;
    std::size_t orders;
    std::size_t levels;  // max of bid and ask peaks
};

std::vector<Peak> read_peaks(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    std::getline(in, line);  // header
    std::vector<Peak> out;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[5];
        for (auto& x : f) std::getline(ss, x, ',');
        out.push_back({static_cast<std::uint16_t>(std::stoul(f[0])), std::stoull(f[2]),
                       std::max(std::stoull(f[3]), std::stoull(f[4]))});
    }
    return out;
}

// Histogram of tick counts: exact up to kBuckets-1 ticks, plus an exact max.
struct Histogram {
    static constexpr std::size_t kBuckets = 1 << 16;
    std::vector<std::uint64_t> buckets = std::vector<std::uint64_t>(kBuckets, 0);
    std::uint64_t count = 0, sum = 0, max = 0;

    void add(std::uint64_t t) noexcept {
        ++buckets[t < kBuckets ? t : kBuckets - 1];
        ++count;
        sum += t;
        if (t > max) max = t;
    }
    // Smallest tick value v with at least p of the samples <= v.
    std::uint64_t percentile(double p) const {
        if (count == 0) return 0;
        const auto need = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(count)));
        std::uint64_t seen = 0;
        for (std::size_t v = 0; v < kBuckets; ++v) {
            seen += buckets[v];
            if (seen >= need) return v == kBuckets - 1 ? max : v;
        }
        return max;
    }
    double mean() const { return count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0; }
};

struct LatencyProbe {
    std::array<Histogram, kOpKinds> hist;
    template <typename F>
    decltype(auto) operator()(OpKind k, F&& op) {
        const std::uint64_t t0 = bench::ticks();
        auto r = op();
        const std::uint64_t t1 = bench::ticks();
        hist[static_cast<std::size_t>(k)].add(t1 - t0);
        return r;
    }
};

struct LatencyStats {
    double p50 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;  // ns
    std::uint64_t count = 0;
};

LatencyStats summarize(const Histogram& h, double ns) {
    return {static_cast<double>(h.percentile(0.50)) * ns, static_cast<double>(h.percentile(0.99)) * ns,
            static_cast<double>(h.percentile(0.999)) * ns, static_cast<double>(h.max) * ns, h.mean() * ns, h.count};
}

struct PassResult {
    double book_s = 0;
    double inflate_s = 0;
    double wall_s = 0;
    std::uint64_t messages = 0;
    std::uint64_t book_messages = 0;
    std::uint64_t allocations = 0;
    std::uint64_t digest = 0;       // combined hash of every book at every chunk boundary
    std::uint64_t checkpoints = 0;  // how many chunk boundaries were hashed
    std::uint64_t errors = 0;  // book calls that returned something other than ok
    std::array<LatencyStats, kOpKinds> latency{};
};

template <typename Book, typename Probe>
PassResult run_pass(const std::string& path, const std::vector<Peak>& peaks, std::vector<std::uint8_t>& buf,
                    double ns_per_tick) {
    PassResult r;
    const auto w0 = Clock::now();
    auto replay = std::make_unique<ItchReplay<Book, Probe>>();
    for (const Peak& p : peaks) replay->books().get(p.locate).reserve(p.orders, p.levels);
    io::GzipFileSource src(path);

    const std::size_t cap = buf.size();
    std::size_t head = 0, tail = 0;
    double book = 0, inflate = 0;
    for (;;) {
        const auto t0 = Clock::now();
        bool eof = false;
        while (tail < cap) {
            const std::size_t got = src.read(buf.data() + tail, cap - tail);
            if (got == 0) {
                eof = true;
                break;
            }
            tail += got;
        }
        const auto t1 = Clock::now();
        g_counting = true;
        const auto t2 = Clock::now();
        std::uint64_t msgs = 0;
        while (tail - head >= 2) {
            const std::size_t len = itch::load_be16(buf.data() + head);
            if (tail - head < 2 + len) break;
            itch::dispatch(buf.data() + head + 2, len, *replay);
            head += 2 + len;
            ++msgs;
        }
        const auto t3 = Clock::now();
        g_counting = false;
        r.messages += msgs;
        inflate += std::chrono::duration<double>(t1 - t0).count();
        book += std::chrono::duration<double>(t3 - t2).count();
        // Hash every book at this chunk boundary (outside the timed segment),
        // so the engines are compared at many points of the day, not only at
        // the end, when Nasdaq has deleted every order and all books are empty.
        r.digest = (r.digest ^ book_digest(replay->books())) * 0x100000001b3ULL;
        ++r.checkpoints;
        if (eof) {
            if (head != tail) throw std::runtime_error("file ends in the middle of a message");
            break;
        }
        std::memmove(buf.data(), buf.data() + head, tail - head);
        tail -= head;
        head = 0;
    }
    r.wall_s = std::chrono::duration<double>(Clock::now() - w0).count();
    r.book_s = book;
    r.inflate_s = inflate;
    r.allocations = g_allocations;
    g_allocations = 0;
    r.book_messages = replay->book_messages();
    for (const auto& kind : replay->results())
        for (std::size_t s = 1; s < kStatusCount; ++s) r.errors += kind[s];
    if constexpr (std::is_same_v<Probe, LatencyProbe>) {
        for (std::size_t k = 0; k < kOpKinds; ++k) r.latency[k] = summarize(replay->probe().hist[k], ns_per_tick);
    }
    return r;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void print_machine_readable(const char* key, double v) { std::printf("<!-- %s=%.6f -->\n", key, v); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: book_replay <day.gz> <book_peaks.csv> --engine fast|reference --mode throughput|latency "
                     "[--passes 3] [--warmup 1] [--chunk-mib 1024]\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::string peaks_path = argv[2];
    std::string engine = "fast", mode = "throughput";
    int passes = 3, warmups = 1;
    std::size_t chunk_mib = 1024;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--engine") engine = v;
        else if (a == "--mode") mode = v;
        else if (a == "--passes") passes = std::stoi(v);
        else if (a == "--warmup") warmups = std::stoi(v);
        else if (a == "--chunk-mib") chunk_mib = std::stoul(v);
        else {
            std::cerr << "book_replay: unknown option " << a << "\n";
            return 2;
        }
    }
    const bool latency = mode == "latency";
    const double ns = bench::ns_per_tick();

    try {
        const auto peaks = read_peaks(peaks_path);
        std::vector<std::uint8_t> buf(chunk_mib << 20);
        std::memset(buf.data(), 0, buf.size());  // touch every page before timing

        auto one = [&]() -> PassResult {
            if (engine == "reference") {
                return latency ? run_pass<ReferenceBook, LatencyProbe>(path, peaks, buf, ns)
                               : run_pass<ReferenceBook, NoProbe>(path, peaks, buf, ns);
            }
            return latency ? run_pass<FastBook, LatencyProbe>(path, peaks, buf, ns)
                           : run_pass<FastBook, NoProbe>(path, peaks, buf, ns);
        };

        for (int i = 0; i < warmups; ++i) {
            const auto w = one();
            std::fprintf(stderr, "%s %s warm-up %d: book %.2f s (discarded)\n", engine.c_str(), mode.c_str(), i + 1, w.book_s);
        }
        std::vector<PassResult> res;
        for (int i = 0; i < passes; ++i) {
            res.push_back(one());
            std::fprintf(stderr, "%s %s pass %d: book %.2f s, inflate %.2f s\n", engine.c_str(), mode.c_str(), i + 1,
                         res.back().book_s, res.back().inflate_s);
            if (res.back().digest != res.front().digest) throw std::runtime_error("books differ between passes");
        }

        const PassResult& f = res.front();
        std::printf("Engine `%s`, mode `%s`: %llu messages, %llu book messages, %llu book calls returned an error; "
                    "hash of every book at %llu points of the day (each 1 GiB chunk boundary and the end) `%016llx`.\n\n",
                    engine.c_str(), mode.c_str(), static_cast<unsigned long long>(f.messages),
                    static_cast<unsigned long long>(f.book_messages), static_cast<unsigned long long>(f.errors),
                    static_cast<unsigned long long>(f.checkpoints), static_cast<unsigned long long>(f.digest));
        std::printf("<!-- digest=%016llx -->\n", static_cast<unsigned long long>(f.digest));

        if (!latency) {
            std::printf("| Pass | Book s | M msg/s (all) | M book msg/s | ns per book msg | Inflate s | Wall s | Heap "
                        "allocations in book segments |\n|---|---:|---:|---:|---:|---:|---:|---:|\n");
            auto row = [&](const std::string& label, const PassResult& p) {
                std::printf("| %s | %.2f | %.1f | %.1f | %.1f | %.2f | %.2f | %llu |\n", label.c_str(), p.book_s,
                            static_cast<double>(p.messages) / p.book_s / 1e6,
                            static_cast<double>(p.book_messages) / p.book_s / 1e6,
                            p.book_s * 1e9 / static_cast<double>(p.book_messages), p.inflate_s, p.wall_s,
                            static_cast<unsigned long long>(p.allocations));
            };
            for (std::size_t i = 0; i < res.size(); ++i) row(std::to_string(i + 1), res[i]);
            std::vector<PassResult> sorted = res;
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.book_s < b.book_s; });
            row("**median**", sorted[sorted.size() / 2]);
            print_machine_readable("median_book_s", sorted[sorted.size() / 2].book_s);
        } else {
            // Timer overhead: back-to-back reads.
            Histogram overhead;
            for (int i = 0; i < 10'000'000; ++i) {
                const std::uint64_t a = bench::ticks();
                const std::uint64_t b = bench::ticks();
                overhead.add(b - a);
            }
            const auto oh = summarize(overhead, ns);
            std::printf("Clock: `%s`, %.3f ns per tick. Timer overhead (10,000,000 back-to-back reads): p50 %.1f ns, "
                        "p99 %.1f ns, p99.9 %.1f ns, mean %.2f ns.\n\n",
                        bench::clock_name(), ns, oh.p50, oh.p99, oh.p999, oh.mean);
            std::printf("| Call | Pass | Count | p50 ns | p99 ns | p99.9 ns | max ns | mean ns |\n"
                        "|---|---|---:|---:|---:|---:|---:|---:|\n");
            for (std::size_t k = 0; k < kOpKinds; ++k) {
                std::vector<double> p50, p99, p999, mx, mean;
                for (std::size_t i = 0; i < res.size(); ++i) {
                    const auto& s = res[i].latency[k];
                    std::printf("| %s | %zu | %llu | %.1f | %.1f | %.1f | %.0f | %.2f |\n", to_string(static_cast<OpKind>(k)),
                                i + 1, static_cast<unsigned long long>(s.count), s.p50, s.p99, s.p999, s.max, s.mean);
                    p50.push_back(s.p50);
                    p99.push_back(s.p99);
                    p999.push_back(s.p999);
                    mx.push_back(s.max);
                    mean.push_back(s.mean);
                }
                std::printf("| **%s** | **median** | %llu | **%.1f** | **%.1f** | **%.1f** | %.0f | %.2f |\n",
                            to_string(static_cast<OpKind>(k)), static_cast<unsigned long long>(res[0].latency[k].count),
                            median(p50), median(p99), median(p999), median(mx), median(mean));
                const std::string key = std::string("k") + std::to_string(k);
                print_machine_readable((key + "_p50").c_str(), median(p50));
                print_machine_readable((key + "_p99").c_str(), median(p99));
                print_machine_readable((key + "_p999").c_str(), median(p999));
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "book_replay: " << e.what() << "\n";
        return 1;
    }
}
