#include "tickforge/io/byte_source.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace tickforge::io {

namespace {
std::FILE* open_or_throw(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) throw std::runtime_error("cannot open " + path);
    return f;
}
}  // namespace

// ---- FileSource ---------------------------------------------------------------

FileSource::FileSource(const std::string& path) : file_(open_or_throw(path)), path_(path) {
    size_ = std::filesystem::file_size(path);
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
}

FileSource::~FileSource() {
    if (file_ != nullptr) std::fclose(file_);
}

std::size_t FileSource::read(std::uint8_t* dst, std::size_t n) {
    const std::size_t got = std::fread(dst, 1, n, file_);
    if (got < n && std::ferror(file_)) throw std::runtime_error("read error on " + path_);
    return got;
}

void FileSource::seek(std::uint64_t offset) {
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0)
        throw std::runtime_error("seek failed on " + path_);
}

// ---- GzipFileSource -------------------------------------------------------------

GzipFileSource::GzipFileSource(const std::string& path, std::size_t input_buffer_size)
    : file_(open_or_throw(path)), path_(path), in_(input_buffer_size) {
    compressed_size_ = std::filesystem::file_size(path);
    // windowBits 15 + 16: expect a gzip header/trailer (not raw zlib).
    if (inflateInit2(&zs_, 15 + 16) != Z_OK) {
        std::fclose(file_);
        throw std::runtime_error("inflateInit2 failed");
    }
}

GzipFileSource::~GzipFileSource() {
    inflateEnd(&zs_);
    if (file_ != nullptr) std::fclose(file_);
}

bool GzipFileSource::refill() {
    if (input_eof_) return false;
    const std::size_t got = std::fread(in_.data(), 1, in_.size(), file_);
    if (got < in_.size()) {
        if (std::ferror(file_)) throw std::runtime_error("read error on " + path_);
        input_eof_ = true;
    }
    compressed_read_ += got;
    zs_.next_in = in_.data();
    zs_.avail_in = static_cast<uInt>(got);
    return got > 0;
}

std::size_t GzipFileSource::read(std::uint8_t* dst, std::size_t n) {
    if (done_ || n == 0) return 0;
    zs_.next_out = dst;
    zs_.avail_out = static_cast<uInt>(n);

    while (zs_.avail_out > 0) {
        if (zs_.avail_in == 0 && !refill()) {
            // No more compressed input.
            if (member_open_) throw std::runtime_error("truncated gzip file: " + path_);
            done_ = true;
            break;
        }
        member_open_ = true;
        const int rc = inflate(&zs_, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            // One gzip member finished (its CRC32/length trailer verified).
            // If more bytes follow, they are another member: reset and go on.
            member_open_ = false;
            if (zs_.avail_in == 0 && !refill()) {
                done_ = true;
                break;
            }
            if (inflateReset(&zs_) != Z_OK) throw std::runtime_error("inflateReset failed");
        } else if (rc == Z_BUF_ERROR) {
            // No progress possible: needs more input (loop refills) or output.
            continue;
        } else if (rc != Z_OK) {
            throw std::runtime_error(std::string("gzip decode error in ") + path_ + ": " +
                                     (zs_.msg != nullptr ? zs_.msg : "unknown"));
        }
    }
    return n - zs_.avail_out;
}

std::unique_ptr<ByteSource> open_byte_source(const std::string& path) {
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0)
        return std::make_unique<GzipFileSource>(path);
    return std::make_unique<FileSource>(path);
}

}  // namespace tickforge::io
