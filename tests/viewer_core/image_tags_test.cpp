#include "viewer_core/image_tags.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using viewer_core::ImageTags;
using viewer_core::makeOverlays;
using viewer_core::Overlay;
using viewer_core::Point;

TEST(ImageTags, OverlaysCompareByContent) {
    const ImageTags first{.overlays = makeOverlays({Point{{1, 2}, "a"}})};
    const ImageTags same{.overlays = makeOverlays({Point{{1, 2}, "a"}})};
    const ImageTags other{.overlays = makeOverlays({Point{{1, 2}, "b"}})};

    EXPECT_NE(first.overlays, same.overlays); // two lists, equal content
    EXPECT_EQ(first, same);
    EXPECT_NE(first, other);
    EXPECT_NE(first, ImageTags{});
}

TEST(ImageTags, AnEmptyListIsNoList) {
    EXPECT_EQ(makeOverlays({}), nullptr);
    EXPECT_EQ(ImageTags{.overlays = makeOverlays({})}, ImageTags{});
    EXPECT_TRUE(ImageTags{}.overlayList().empty());
}

TEST(ImageTags, OtherFieldsStillCount) {
    EXPECT_NE((ImageTags{.valueRange = {0, 1}}), ImageTags{});
    EXPECT_NE((ImageTags{.transfer = viewer_core::Transfer::Hlg}), ImageTags{});
    EXPECT_NE((ImageTags{.primaries = viewer_core::Primaries::Bt709}), ImageTags{});
}

TEST(ImageTags, CopiesShareTheList) {
    const ImageTags tags{.overlays = makeOverlays(std::vector<Overlay>(256, Point{{1, 2}}))};
    const ImageTags copy = tags;

    EXPECT_EQ(copy.overlays.get(), tags.overlays.get());
}

} // namespace
