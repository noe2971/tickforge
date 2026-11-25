#pragma once
// Pull-based byte streams. The ITCH framer reads from a ByteSource in large
// chunks, so it never needs the whole file in memory and doesn't care whether
// bytes come from a plain file or from gzip decompression.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <zlib.h>

namespace tickforge::io {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Copies up to n bytes into dst and returns how many were copied.
    // Returns 0 only at end of stream. Throws std::runtime_error on I/O or
    // format errors.
    virtual std::size_t read(std::uint8_t* dst, std::size_t n) = 0;
};

// Uncompressed file read with stdio buffering.
class FileSource final : public ByteSource {
public:
    explicit FileSource(const std::string& path);
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    std::size_t read(std::uint8_t* dst, std::size_t n) override;
    // Moves the read position to an absolute byte offset.
    void seek(std::uint64_t offset);
    std::uint64_t size() const noexcept { return size_; }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    std::uint64_t size_ = 0;
};

// Streams the decompressed contents of a .gz file using zlib, without ever
// writing the decompressed data to disk. Handles multi-member gzip files
// (several gzip streams concatenated, which `gzip` and `cat a.gz b.gz` make).
// Each member's CRC32 and length trailer is checked by zlib; a truncated or
// corrupt file throws.
class GzipFileSource final : public ByteSource {
public:
    explicit GzipFileSource(const std::string& path, std::size_t input_buffer_size = 1 << 20);
    ~GzipFileSource() override;
    GzipFileSource(const GzipFileSource&) = delete;
    GzipFileSource& operator=(const GzipFileSource&) = delete;

    std::size_t read(std::uint8_t* dst, std::size_t n) override;

    std::uint64_t compressed_bytes_read() const noexcept { return compressed_read_; }
    std::uint64_t compressed_size() const noexcept { return compressed_size_; }

private:
    bool refill();

    std::FILE* file_ = nullptr;
    std::string path_;
    z_stream zs_{};
    std::vector<std::uint8_t> in_;
    std::uint64_t compressed_read_ = 0;
    std::uint64_t compressed_size_ = 0;
    bool input_eof_ = false;
    bool member_open_ = false;  // inside a gzip member that hasn't ended yet
    bool done_ = false;
};

// Opens path as gzip if it ends in ".gz", otherwise as a plain file.
std::unique_ptr<ByteSource> open_byte_source(const std::string& path);

}  // namespace tickforge::io
