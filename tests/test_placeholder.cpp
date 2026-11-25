#include <gtest/gtest.h>

#include "tickforge/version.hpp"

TEST(Version, PatchIsZero) {
    EXPECT_EQ(tickforge::kVersionPatch, 0);
}
