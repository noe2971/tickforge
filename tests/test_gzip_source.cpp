// GzipFileSource must stream the exact decompressed bytes, including across
// gzip members, and must reject truncated input.

#include <gtest/gtest.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "itch_test_util.hpp"
#include "tickforge/io/byte_source.hpp"

using namespace tickforge;
using tickforge::test::TempFile;

namespace {

std::vector<std::uint8_t> random_bytes(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> v(n);
    // Mix of compressible runs and noise.
    for (std::size_t i = 0; i < n; ++i) v[i] = (i / 97) % 3 == 0 ? static_cast<std::uint8_t>(rng()) : 'x';
    return v;
}

// mode "wb" starts a new file; "ab" appends a new gzip member.
void write_gz(const std::string& path, const std::vector<std::uint8_t>& data, const char* mode) {
    gzFile f = gzopen(path.c_str(), mode);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(gzwrite(f, data.data(), static_cast<unsigned>(data.size())), static_cast<int>(data.size()));
    ASSERT_EQ(gzclose(f), Z_OK);
}

std::vector<std::uint8_t> read_all(io::ByteSource& src, std::size_t chunk) {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> buf(chunk);
    while (std::size_t n = src.read(buf.data(), buf.size())) out.insert(out.end(), buf.begin(), buf.begin() + n);
    return out;
}

}  // namespace

TEST(GzipSource, SingleMemberRoundTrip) {
    TempFile tmp("single.gz");
    const auto data = random_bytes(3'000'000, 1);
    write_gz(tmp.path, data, "wb");
    // Tiny input buffer forces many refills.
    io::GzipFileSource src(tmp.path, 4096);
    EXPECT_EQ(read_all(src, 10007), data);
    EXPECT_EQ(src.compressed_bytes_read(), std::filesystem::file_size(tmp.path));
}

TEST(GzipSource, MultiMemberFileIsReadToTheEnd) {
    TempFile tmp("multi.gz");
    const auto a = random_bytes(500'000, 2);
    const auto b = random_bytes(700'000, 3);
    write_gz(tmp.path, a, "wb");
    write_gz(tmp.path, b, "ab");
    std::vector<std::uint8_t> both = a;
    both.insert(both.end(), b.begin(), b.end());
    io::GzipFileSource src(tmp.path, 1000);
    EXPECT_EQ(read_all(src, 65536), both);
}

TEST(GzipSource, TruncatedFileThrows) {
    TempFile tmp("trunc.gz");
    write_gz(tmp.path, random_bytes(200'000, 4), "wb");
    const auto size = std::filesystem::file_size(tmp.path);
    std::filesystem::resize_file(tmp.path, size - 10);  // drop the trailer + some data
    io::GzipFileSource src(tmp.path);
    EXPECT_THROW(read_all(src, 65536), std::runtime_error);
}

TEST(GzipSource, CorruptDataThrows) {
    TempFile tmp("corrupt.gz");
    write_gz(tmp.path, random_bytes(200'000, 5), "wb");
    {
        std::fstream f(tmp.path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(100);
        const char junk[64] = {};
        f.write(junk, sizeof junk);
    }
    io::GzipFileSource src(tmp.path);
    EXPECT_THROW(read_all(src, 65536), std::runtime_error);
}

TEST(GzipSource, OpenByteSourcePicksByExtension) {
    TempFile gz("pick.gz");
    TempFile raw("pick.bin");
    const std::vector<std::uint8_t> data = {1, 2, 3, 4, 5};
    write_gz(gz.path, data, "wb");
    {
        std::ofstream f(raw.path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    auto a = io::open_byte_source(gz.path);
    auto b = io::open_byte_source(raw.path);
    EXPECT_EQ(read_all(*a, 2), data);
    EXPECT_EQ(read_all(*b, 2), data);
}
