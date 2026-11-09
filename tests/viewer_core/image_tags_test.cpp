#include "viewer_core/image_tags.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <variant>
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

// A snapshot of part of a frame keeps its shapes where they were on the frame: moved by the cut's corner.
TEST(ImageTags, TagsForACropMoveTheShapes) {
    const ImageTags tags{
        .valueRange = {0, 1023},
        .transfer = viewer_core::Transfer::Hlg,
        .overlays = makeOverlays({Point{{110, 220}, "p"}, viewer_core::Rectangle{{100, 200, 30, 40}, "r"},
                                  viewer_core::Circle{{150, 250}, 7, "c"},
                                  viewer_core::Polygon{{{100, 200}, {130, 200}, {100, 240}}, "g"}})};

    const ImageTags cropped = viewer_core::tagsForCrop(tags, cv::Point(100, 200));

    EXPECT_EQ(cropped.valueRange, tags.valueRange);
    EXPECT_EQ(cropped.transfer, tags.transfer);
    const std::vector<Overlay> expected{Point{{10, 20}, "p"}, viewer_core::Rectangle{{0, 0, 30, 40}, "r"},
                                        viewer_core::Circle{{50, 50}, 7, "c"},
                                        viewer_core::Polygon{{{0, 0}, {30, 0}, {0, 40}}, "g"}};
    EXPECT_EQ(cropped.overlayList(), expected);
    EXPECT_EQ(tags.overlayList().size(), 4U); // the frame's own list is unchanged
    EXPECT_EQ(std::get<Point>(tags.overlayList()[0]).at, cv::Point2d(110, 220));
    EXPECT_EQ(viewer_core::tagsForCrop(ImageTags{}, cv::Point(5, 5)), ImageTags{});
}

TEST(ImageTags, CopiesShareTheList) {
    const ImageTags tags{.overlays = makeOverlays(std::vector<Overlay>(256, Point{{1, 2}}))};
    const ImageTags copy = tags;

    EXPECT_EQ(copy.overlays.get(), tags.overlays.get());
}

} // namespace
