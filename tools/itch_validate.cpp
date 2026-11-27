// itch_validate: replay validation of a full ITCH day (PROGRESS.md, Rule D).
//
//   itch_validate <day.gz|day.itch|file.tfc> [output_dir=results] [label] [FOCUS_SYMBOL...]
//
// Writes, into output_dir:
//   replay_validation_<label>.md   every check, every exception with examples
//   itch_<label>_book_peaks.csv    per-stock peak sizes (pre-sizes the benchmark)
// label defaults to the first 8 characters of the input file name.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "tickforge/book/replay_validator.hpp"
#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/stream.hpp"

namespace fs = std::filesystem;
using namespace tickforge;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: itch_validate <input> [output_dir] [label] [FOCUS_SYMBOL...]\n";
        return 2;
    }
    const std::string input = argv[1];
    const fs::path out_dir = argc > 2 ? argv[2] : "results";
    const std::string label = argc > 3 ? argv[3] : fs::path(input).filename().string().substr(0, 8);
    std::vector<std::string> focus(argv + std::min(argc, 4), argv + argc);
    if (focus.empty()) focus = {"AAPL", "MSFT", "INTC", "COLB"};

    try {
        std::unique_ptr<io::ByteSource> src;
        if (input.size() > 4 && input.compare(input.size() - 4, 4, ".tfc") == 0) {
            src = std::make_unique<itch::CompactReader>(input);
        } else {
            src = io::open_byte_source(input);
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto v = std::make_unique<book::ReplayValidator>();
        const auto st = itch::parse_stream(*src, *v);
        v->finish();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        fs::create_directories(out_dir);
        const fs::path md_path = out_dir / ("replay_validation_" + label + ".md");
        const fs::path csv_path = out_dir / ("itch_" + label + "_book_peaks.csv");
        {
            std::ofstream md(md_path);
            v->write_markdown(md, fs::path(input).filename().string(), focus);
        }
        {
            std::ofstream csv(csv_path);
            v->write_peaks_csv(csv);
        }
        std::cout << "validated " << st.messages << " messages in " << secs << " s; " << v->issue_total()
                  << " exceptions recorded\n";
        for (std::size_t i = 0; i < book::kIssueCount; ++i) {
            const auto& log = v->issue(static_cast<book::Issue>(i));
            if (log.count != 0) std::cout << "  " << book::to_string(static_cast<book::Issue>(i)) << ": " << log.count << "\n";
        }
        std::cout << "wrote " << md_path.string() << "\nwrote " << csv_path.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "itch_validate: " << e.what() << "\n";
        return 1;
    }
}
