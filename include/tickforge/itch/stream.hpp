#pragma once
// Streaming framer for the Nasdaq ITCH binary file format.
//
// The file is a sequence of [2-byte big-endian length][message bytes]. We
// read the source in fixed-size chunks into one reusable buffer, hand every
// complete message to a callback, and carry an incomplete message at the end
// of a chunk over to the next chunk. Memory use is the buffer size (1 MiB by
// default) no matter how big the file is.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/endian.hpp"

namespace tickforge::itch {

struct StreamStats {
    std::uint64_t messages = 0;       // framed messages seen
    std::uint64_t bytes = 0;          // uncompressed bytes consumed (incl. length prefixes)
    std::uint64_t unknown_type = 0;   // message type letter not in ITCH 5.0
    std::uint64_t bad_length = 0;     // length prefix != spec length for that type
    std::uint64_t read_ns = 0;        // time spent inside ByteSource::read (decompression + I/O)
    std::uint64_t parse_ns = 0;       // time spent framing + in the callback
    bool stopped = false;             // the callback asked to stop before the end
};

// Smallest buffer that always fits one maximum-size frame (2 + 65535 bytes).
inline constexpr std::size_t kMinFrameBuffer = 2 + 0xFFFF;

// Calls fn(const uint8_t* msg, std::size_t len) for every framed message, in
// file order. `msg` points at the message type byte; it is only valid during
// the call. If fn returns bool, returning false stops the pass early (the
// result then has stopped = true). Throws std::runtime_error if the stream
// ends mid-message.
template <typename Fn>
StreamStats for_each_frame(io::ByteSource& src, Fn&& fn, std::size_t buffer_size = 1 << 20) {
    using clock = std::chrono::steady_clock;
    const std::size_t cap = std::max(buffer_size, kMinFrameBuffer);
    std::vector<std::uint8_t> buf(cap);
    std::size_t head = 0;  // first unconsumed byte
    std::size_t tail = 0;  // one past the last valid byte
    StreamStats stats;

    for (;;) {
        const auto t0 = clock::now();
        const std::size_t got = src.read(buf.data() + tail, cap - tail);
        const auto t1 = clock::now();
        tail += got;

        while (tail - head >= 2) {
            const std::size_t len = load_be16(buf.data() + head);
            if (tail - head < 2 + len) break;  // message continues in the next chunk
            const std::uint8_t* msg = buf.data() + head + 2;
            head += 2 + len;
            ++stats.messages;
            if constexpr (std::is_same_v<std::invoke_result_t<Fn&, const std::uint8_t*, std::size_t>, bool>) {
                if (!fn(msg, len)) {
                    stats.stopped = true;
                    break;
                }
            } else {
                fn(msg, len);
            }
        }
        const auto t2 = clock::now();
        stats.read_ns += static_cast<std::uint64_t>(std::chrono::nanoseconds(t1 - t0).count());
        stats.parse_ns += static_cast<std::uint64_t>(std::chrono::nanoseconds(t2 - t1).count());
        stats.bytes += head;

        if (stats.stopped) break;
        if (got == 0) {
            if (head != tail) throw std::runtime_error("ITCH stream ends in the middle of a message");
            break;
        }
        // Slide the partial message (at most 65,536 bytes) to the front.
        std::memmove(buf.data(), buf.data() + head, tail - head);
        tail -= head;
        head = 0;
    }
    return stats;
}

// Frames, decodes and dispatches every message to handler h (see NullHandler).
template <typename H>
StreamStats parse_stream(io::ByteSource& src, H& h, std::size_t buffer_size = 1 << 20) {
    std::uint64_t unknown = 0;
    std::uint64_t bad = 0;
    StreamStats s = for_each_frame(
        src,
        [&](const std::uint8_t* msg, std::size_t len) {
            switch (dispatch(msg, len, h)) {
                case DecodeStatus::ok: break;
                case DecodeStatus::unknown_type: ++unknown; break;
                case DecodeStatus::bad_length: ++bad; break;
            }
        },
        buffer_size);
    s.unknown_type = unknown;
    s.bad_length = bad;
    return s;
}

}  // namespace tickforge::itch
