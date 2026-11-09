#include "image_tags_conversion.h"

#include <gtest/gtest.h>

namespace {

TEST(ImageTagsConversion, OverlaysConvertBothWays) {
    const visual_debug::ImageDescription description{
        .valueRange = {0, 1023},
        .overlays = {visual_debug::Polygon{{{1, 2}, {3, 4}, {5, 0.5}}, "MRZ", visual_debug::kOverlayRed},
                     visual_debug::Rectangle{{1, 2, 3, 4}, "Photo"},
                     visual_debug::Circle{{5, 6}, 7, "", visual_debug::kOverlayAmber},
                     visual_debug::Point{{8, 9}, "corner", {1, 2, 3}}}};

    const viewer_core::ImageTags tags = toImageTags(description);

    ASSERT_EQ(tags.overlayList().size(), 4U);
    EXPECT_EQ(std::get<viewer_core::Polygon>(tags.overlayList()[0]),
              (viewer_core::Polygon{{{1, 2}, {3, 4}, {5, 0.5}}, "MRZ", {0xE0, 0x50, 0x50}}));
    EXPECT_EQ(std::get<viewer_core::Rectangle>(tags.overlayList()[1]),
              (viewer_core::Rectangle{{1, 2, 3, 4}, "Photo"}));
    EXPECT_EQ(std::get<viewer_core::Circle>(tags.overlayList()[2]),
              (viewer_core::Circle{{5, 6}, 7, "", {0xF0, 0xB0, 0x40}}));
    EXPECT_EQ(std::get<viewer_core::Point>(tags.overlayList()[3]),
              (viewer_core::Point{{8, 9}, "corner", {1, 2, 3}}));
    EXPECT_EQ(toDescription(tags), description);
}

TEST(ImageTagsConversion, NoOverlaysGiveANullList) {
    const viewer_core::ImageTags tags = toImageTags({});

    EXPECT_EQ(tags.overlays, nullptr);
    EXPECT_TRUE(tags.overlayList().empty());
    EXPECT_TRUE(toDescription(tags).overlays.empty());
}

} // namespace
