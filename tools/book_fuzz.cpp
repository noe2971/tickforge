// book_fuzz: the full differential fuzz test (PROGRESS.md, Rule C).
//
//   book_fuzz [--short N] [--short-events M] [--long K] [--long-events L]
//             [--seed S] [--threads T] [--out FILE]
//
// Runs N short streams (seeds S .. S+N-1) and K long streams (seeds
// S+1e9 .. S+1e9+K-1) through FastBook and ReferenceBook, comparing results,
// fills and the complete book after every event. Streams are independent, so
// they are spread over T threads; the result does not depend on T. Stops at
// the first mismatch and prints how to reproduce it. Exit status 0 = all
// streams agreed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tickforge/book/differential.hpp"

using namespace tickforge::book;

namespace {

const char* build_kind() {
#if defined(TICKFORGE_ASAN)
    const bool asan = true;
#else
    const bool asan = false;
#endif
#if defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
    const bool ubsan = true;
#else
    const bool ubsan = false;
#endif
#else
    const bool ubsan = false;
#endif
#if defined(NDEBUG)
    const bool opt = true;
#else
    const bool opt = false;
#endif
    if (asan && ubsan) return opt ? "optimised + ASan + UBSan" : "debug + ASan + UBSan";
    if (asan) return opt ? "optimised + ASan" : "debug + ASan";
    return opt ? "release (NDEBUG)" : "debug";
}

struct Batch {
    std::uint64_t first_seed;
    std::uint64_t streams;
    fuzz::Config cfg;
    const char* name;
};

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t n_short = 1'000'000, short_events = 100, n_long = 20, long_events = 500'000;
    std::uint64_t seed = 20261127;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::string out_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--short") n_short = std::stoull(next());
            else if (a == "--short-events") short_events = std::stoull(next());
            else if (a == "--long") n_long = std::stoull(next());
            else if (a == "--long-events") long_events = std::stoull(next());
            else if (a == "--seed") seed = std::stoull(next());
            else if (a == "--threads") threads = static_cast<unsigned>(std::stoul(next()));
            else if (a == "--out") out_path = next();
            else throw std::runtime_error("unknown option " + a);
        } catch (const std::exception& e) {
            std::cerr << "book_fuzz: " << e.what() << "\n";
            return 2;
        }
    }

    fuzz::Config short_cfg = fuzz::short_stream_config();
    short_cfg.events = short_events;
    const fuzz::Config long_cfg = fuzz::long_stream_config(long_events);
    const std::vector<Batch> batches{{seed, n_short, short_cfg, "short"},
                                     {seed + 1'000'000'000ULL, n_long, long_cfg, "long"}};

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<fuzz::Counters> per_batch(batches.size());
    std::optional<fuzz::Mismatch> failure;
    std::mutex mu;
    std::atomic<bool> stop{false};

    for (std::size_t bi = 0; bi < batches.size(); ++bi) {
        const Batch& b = batches[bi];
        std::atomic<std::uint64_t> next_stream{0}, done{0};
        std::vector<fuzz::Counters> local(threads);
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                for (;;) {
                    if (stop.load(std::memory_order_relaxed)) return;
                    const std::uint64_t i = next_stream.fetch_add(1);
                    if (i >= b.streams) return;
                    auto m = fuzz::run_stream(b.first_seed + i, b.cfg, local[t]);
                    if (m) {
                        std::lock_guard lock(mu);
                        if (!failure) failure = std::move(m);
                        stop = true;
                        return;
                    }
                    const auto d = done.fetch_add(1) + 1;
                    if (d % 100000 == 0 || (b.streams < 1000 && d == b.streams)) {
                        std::lock_guard lock(mu);
                        std::fprintf(stderr, "%s streams: %llu / %llu\n", b.name, static_cast<unsigned long long>(d),
                                     static_cast<unsigned long long>(b.streams));
                    }
                }
            });
        }
        for (auto& th : pool) th.join();
        for (const auto& c : local) per_batch[bi].merge(c);
        if (failure) break;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (failure) {
        std::cerr << "MISMATCH: seed " << failure->seed << ", event " << failure->event << ": " << failure->detail << "\n";
        return 1;
    }

    fuzz::Counters total;
    for (const auto& c : per_batch) total.merge(c);
    std::ostringstream md;
    md << "# Differential fuzz run — " << build_kind() << "\n\n";
    md << "Generated by `tools/book_fuzz` (method: `PROGRESS.md`, Rule C). FastBook vs ReferenceBook; after every\n"
          "event the returned values, the fills and the complete book (every level and every order in queue\n"
          "order) were compared.\n\n";
    md << "| | Short streams | Long streams | Total |\n|---|---:|---:|---:|\n";
    md << "| Streams | " << per_batch[0].streams << " | " << per_batch[1].streams << " | " << total.streams << " |\n";
    md << "| Events per stream | " << short_cfg.events << " | " << long_cfg.events << " | |\n";
    md << "| First seed | " << batches[0].first_seed << " | " << batches[1].first_seed << " | |\n";
    md << "| Events compared | " << per_batch[0].events << " | " << per_batch[1].events << " | " << total.events << " |\n";
    md << "| Fills compared | " << per_batch[0].fills << " | " << per_batch[1].fills << " | " << total.fills << " |\n";
    md << "| Largest book (orders) | " << per_batch[0].max_orders << " | " << per_batch[1].max_orders << " | |\n";
    md << "| Largest book (levels, both sides) | " << per_batch[0].max_levels << " | " << per_batch[1].max_levels
       << " | |\n";
    md << "| Mismatches | 0 | 0 | **0** |\n\n";
    md << "Wall time " << static_cast<long long>(secs) << " s on " << threads << " threads.\n\n";
    md << "Calls by kind and result (reference engine's answer; the fast engine gave the same every time):\n\n";
    md << "| Call |";
    for (std::size_t s = 0; s < kStatusCount; ++s) md << " " << to_string(static_cast<Status>(s)) << " |";
    md << "\n|---|";
    for (std::size_t s = 0; s < kStatusCount; ++s) md << "---:|";
    md << "\n";
    for (std::size_t op = 0; op < fuzz::kOps; ++op) {
        md << "| " << fuzz::to_string(static_cast<fuzz::Op>(op)) << " |";
        for (std::size_t s = 0; s < kStatusCount; ++s) md << " " << total.by_op[op][s] << " |";
        md << "\n";
    }
    std::cout << md.str();
    if (!out_path.empty()) std::ofstream(out_path) << md.str();
    return 0;
}
