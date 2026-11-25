#include <gtest/gtest.h>

#include "tickforge/itch/endian.hpp"

using namespace tickforge::itch;

TEST(ItchEndian, LoadsAreBigEndian) {
    const std::uint8_t b[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    EXPECT_EQ(load_be16(b), 0x0102u);
    EXPECT_EQ(load_be32(b), 0x01020304u);
    EXPECT_EQ(load_be48(b), 0x010203040506ULL);
    EXPECT_EQ(load_be64(b), 0x0102030405060708ULL);
    // Unaligned address (ITCH fields sit at odd offsets).
    EXPECT_EQ(load_be32(b + 1), 0x02030405u);
    EXPECT_EQ(load_be64(b + 1), 0x0203040506070809ULL);
}

TEST(ItchEndian, Max48BitTimestamp) {
    const std::uint8_t b[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(load_be48(b), 0xFFFFFFFFFFFFULL);
}

TEST(ItchEndian, StoreThenLoadRoundTrips) {
    std::uint8_t b[8];
    store_be16(b, 0xABCD);
    EXPECT_EQ(b[0], 0xAB);
    EXPECT_EQ(b[1], 0xCD);
    store_be64(b, 0x1122334455667788ULL);
    EXPECT_EQ(b[0], 0x11);
    EXPECT_EQ(b[7], 0x88);
    EXPECT_EQ(load_be64(b), 0x1122334455667788ULL);
}
