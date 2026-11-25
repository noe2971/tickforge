// itch_filter: copy the messages for a few symbols into a compact file.
//
//   itch_filter <input.gz|input.itch> <output.tfc> SYMBOL [SYMBOL...]
//
// Keeps, in original order and byte-for-byte, every message for the chosen
// symbols plus the market-wide messages (system events, circuit breakers).
// See include/tickforge/itch/symbol_filter.hpp for the exact rule and
// include/tickforge/itch/compact_file.hpp for the file layout.

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/endian.hpp"
#include "tickforge/itch/stream.hpp"
#include "tickforge/itch/symbol_filter.hpp"

using namespace tickforge;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: itch_filter <input> <output.tfc> SYMBOL [SYMBOL...]\n";
        return 2;
    }
    const std::string input = argv[1];
    const std::string output = argv[2];
    const std::vector<std::string> symbols(argv + 3, argv + argc);

    try {
        auto src = io::open_byte_source(input);
        itch::SymbolFilter filter(symbols);
        itch::CompactWriter writer(output, std::filesystem::path(input).filename().string(), symbols);
        std::map<std::uint16_t, std::uint64_t> kept_by_locate;

        const auto st = itch::for_each_frame(*src, [&](const std::uint8_t* msg, std::size_t len) {
            if (filter.keep(msg, len)) {
                writer.write(msg, len);
                ++kept_by_locate[itch::load_be16(msg + 1)];
            }
        });
        for (const auto& f : filter.found()) writer.set_locate(f.symbol, f.locate);
        writer.close();

        std::cout << "read " << st.messages << " messages, kept " << writer.header().message_count << " ("
                  << writer.header().payload_bytes << " payload bytes)\n";
        std::cout << "  locate 0 (market-wide): " << kept_by_locate[0] << "\n";
        for (const auto& s : writer.header().symbols) {
            if (s.locate == 0) {
                std::cout << "  " << s.symbol.view() << ": NOT FOUND in stock directory\n";
            } else {
                std::cout << "  " << s.symbol.view() << " (locate " << s.locate << "): " << kept_by_locate[s.locate]
                          << "\n";
            }
        }
        std::cout << "wrote " << output << " (" << std::filesystem::file_size(output) << " bytes)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "itch_filter: " << e.what() << "\n";
        return 1;
    }
}
