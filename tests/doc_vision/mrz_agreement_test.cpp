#include "doc_vision/mrz_agreement.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace {

using doc_vision::MrzAgreement;

TEST(MrzAgreement, ThreeOfTenAccepts) {
    MrzAgreement agreement;
    EXPECT_FALSE(agreement.add("A", 1));
    EXPECT_FALSE(agreement.add("B", 2));
    EXPECT_FALSE(agreement.add("A", 3));
    EXPECT_FALSE(agreement.add("C", 4));
    EXPECT_EQ(agreement.add("A", 5), std::optional<std::string>("A"));
    EXPECT_EQ(agreement.count(), 3);
}

TEST(MrzAgreement, TwoDoNot) {
    MrzAgreement agreement;
    EXPECT_FALSE(agreement.add("A", 1));
    EXPECT_FALSE(agreement.add("B", 2));
    EXPECT_FALSE(agreement.add("A", 3));
    EXPECT_EQ(agreement.count(), 2);
}

TEST(MrzAgreement, EleventhDropsTheFirst) {
    MrzAgreement agreement;
    EXPECT_FALSE(agreement.add("A", 1));
    for (int i = 1; i <= 9; ++i) {
        EXPECT_FALSE(agreement.add("X" + std::to_string(i), static_cast<std::uint64_t>(i + 1)));
    }
    EXPECT_FALSE(agreement.add("A", 11)); // the first A leaves the window
    EXPECT_FALSE(agreement.add("A", 12));
    EXPECT_EQ(agreement.count(), 2);
    EXPECT_EQ(agreement.size(), 10u);
    EXPECT_EQ(agreement.add("A", 13), std::optional<std::string>("A"));
}

TEST(MrzAgreement, DifferentTextsNeverAddUp) {
    MrzAgreement agreement;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(agreement.add("T" + std::to_string(i), static_cast<std::uint64_t>(i)));
    }
    EXPECT_EQ(agreement.count(), 1);
}

// Three reads of the same pixels are no evidence (spec §4.1).
TEST(MrzAgreement, SameFrameTwiceCountsOnce) {
    MrzAgreement agreement;
    EXPECT_FALSE(agreement.add("A", 1));
    EXPECT_FALSE(agreement.add("A", 1));
    EXPECT_FALSE(agreement.add("A", 1));
    EXPECT_EQ(agreement.size(), 1u);
    EXPECT_EQ(agreement.count(), 1);
    EXPECT_TRUE(agreement.holds(1));
    EXPECT_FALSE(agreement.holds(2));
    EXPECT_FALSE(agreement.add("A", 2));
    EXPECT_EQ(agreement.add("A", 3), std::optional<std::string>("A"));
}

TEST(MrzAgreement, ClearForgets) {
    MrzAgreement agreement;
    EXPECT_FALSE(agreement.add("A", 1));
    EXPECT_FALSE(agreement.add("A", 2));
    agreement.clear();
    EXPECT_EQ(agreement.count(), 0);
    EXPECT_FALSE(agreement.add("A", 3));
    EXPECT_EQ(agreement.count(), 1);
    EXPECT_EQ(agreement.size(), 1u);
}

} // namespace
