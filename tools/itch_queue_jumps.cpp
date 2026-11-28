// itch_queue_jumps: explains E executions of orders that were not first in
// their queue (see include/tickforge/book/queue_jumps.hpp).
//
//   itch_queue_jumps <day.gz|day.itch|file.tfc> <output.md>

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "tickforge/book/queue_jumps.hpp"
#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: itch_queue_jumps <input> <output.md>\n";
        return 2;
    }
    const std::string input = argv[1];
    try {
        std::unique_ptr<io::ByteSource> src;
        if (input.size() > 4 && input.compare(input.size() - 4, 4, ".tfc") == 0) {
            src = std::make_unique<itch::CompactReader>(input);
        } else {
            src = io::open_byte_source(input);
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto a = std::make_unique<book::QueueJumpAnalyzer>();
        const auto st = itch::parse_stream(*src, *a);
        a->finish();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::ofstream md(argv[2]);
        const std::string name = input.substr(input.find_last_of('/') + 1);
        a->write_markdown(md, name);
        std::cout << st.messages << " messages, " << a->executions() << " E executions, " << a->jumps().size()
                  << " queue jumps, " << secs << " s -> " << argv[2] << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
