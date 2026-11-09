#include "card_frame.h"
#include "lab_core/document_outline.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <optional>
#include <vector>

namespace {

using test_support::cardFrame;
using test_support::findsTheCorners;
using test_support::kCardCorners;

TEST(Edges, AreSingleChannelOfTheSameSize) {
    const cv::Mat frame = cardFrame();

    const cv::Mat edges = lab_core::edgesOf(frame);

    EXPECT_EQ(edges.type(), CV_8UC1);
    EXPECT_EQ(edges.size(), frame.size());
    EXPECT_GT(cv::countNonZero(edges), 0);
    EXPECT_EQ(lab_core::edgesOf(cv::Mat(10, 12, CV_8UC1, cv::Scalar(7))).type(), CV_8UC1);
    EXPECT_EQ(lab_core::edgesOf(cv::Mat(10, 12, CV_8UC4, cv::Scalar(1, 2, 3, 4))).type(), CV_8UC1);
}

// An HDR video's frames are 16-bit: their edges are those of the top 8 bits.
TEST(Edges, OfSixteenBitMatchEightBit) {
    const cv::Mat frame8 = cardFrame();
    cv::Mat frame16;
    frame8.convertTo(frame16, CV_16U, 257.0); // 255 becomes 65535

    const cv::Mat edges = lab_core::edgesOf(frame16);

    ASSERT_EQ(edges.type(), CV_8UC1);
    EXPECT_EQ(cv::norm(edges, lab_core::edgesOf(frame8), cv::NORM_INF), 0.0);
}

TEST(DocumentOutline, FindsTheDocumentsFourCorners) {
    const auto outline = lab_core::documentOutline(cardFrame());

    ASSERT_TRUE(outline);
    EXPECT_TRUE(findsTheCorners(*outline, 1, 4.0));
}

TEST(DocumentOutline, FindsItInALargeFrame) {
    const auto outline = lab_core::documentOutline(cardFrame(3));

    ASSERT_TRUE(outline);
    EXPECT_TRUE(findsTheCorners(*outline, 3, 8.0));
}

// The outline is found on a copy 6 times smaller; mapping its corners back must take each copy pixel's
// centre, or the whole outline shifts by 2.5 frame pixels up and to the left. The corners' centroid shows it,
// free of the edge detector's own spread, which is the same on every side.
TEST(DocumentOutline, IsNotShiftedByTheShrinking) {
    // A rectangle, so the edge detector's outward spread is the same on opposite sides and cancels in the
    // centroid: pixels 600 to 3239 across and 480 to 2399 down, whose centre is (1919.5, 1439.5).
    cv::Mat frame(2880, 3840, CV_8UC3, cv::Scalar::all(40)); // found at 640×480
    cv::rectangle(frame, cv::Rect(600, 480, 2640, 1920), cv::Scalar::all(220), cv::FILLED);
    const cv::Point2d expected(1919.5, 1439.5);

    const auto outline = lab_core::documentOutline(frame);

    ASSERT_TRUE(outline);
    ASSERT_EQ(outline->size(), 4U);
    cv::Point2d centroid;
    for (const cv::Point2d& corner : *outline) {
        centroid += corner / 4.0;
    }
    EXPECT_NEAR(centroid.x, expected.x, 1.0);
    EXPECT_NEAR(centroid.y, expected.y, 1.0);
}

// A thumb over one edge, darker than the card, dents its outline; the card is still found. (Fitting the
// corners to the outline's convex hull matters most on real frames: on the passport clips it gave fewer
// misses and half the flicker; this synthetic dent is handled without it too.)
TEST(DocumentOutline, AThumbOverAnEdgeStillFindsTheCard) {
    cv::Mat frame = cardFrame();
    cv::ellipse(frame, cv::Point(115, 230), cv::Size(70, 40), 0, 0, 360, cv::Scalar(60, 80, 110), cv::FILLED);

    const auto outline = lab_core::documentOutline(frame);

    ASSERT_TRUE(outline);
    EXPECT_TRUE(findsTheCorners(*outline, 1, 12.0));
}

// Only a document wholly in view is outlined: one the frame cuts off has an edge that is the frame's.
TEST(DocumentOutline, NoneForACardCutOffByTheFrame) {
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar::all(40));
    cv::fillConvexPoly(frame, std::vector<cv::Point>{{100, 80}, {540, 100}, {520, 479}, {120, 479}},
                       cv::Scalar::all(220));

    EXPECT_FALSE(lab_core::documentOutline(frame));
}

// A page is lighter than what it lies on: a dark shape on a light desk, such as a keyboard, is no document.
TEST(DocumentOutline, NoneForADarkShapeOnALightDesk) {
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar::all(220));
    cv::fillConvexPoly(frame, kCardCorners, cv::Scalar::all(40));

    EXPECT_FALSE(lab_core::documentOutline(frame));
}

// Four corners with one side far shorter than the others, as when a hand joins a page, are no document.
TEST(DocumentOutline, NoneForASliver) {
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar::all(40));
    cv::fillConvexPoly(frame, std::vector<cv::Point>{{60, 60}, {580, 90}, {590, 130}, {70, 420}},
                       cv::Scalar::all(220));

    EXPECT_FALSE(lab_core::documentOutline(frame));
}

TEST(DocumentOutline, NoneInAPlainFrame) {
    EXPECT_FALSE(lab_core::documentOutline(cv::Mat(480, 640, CV_8UC3, cv::Scalar::all(128))));
}

TEST(DocumentOutline, NoneForASmallShape) {
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar::all(40));
    cv::rectangle(frame, cv::Rect(300, 200, 20, 20), cv::Scalar::all(220), cv::FILLED);

    EXPECT_FALSE(lab_core::documentOutline(frame));
}

TEST(DocumentOutline, SixteenBitFramesHaveOutlinesToo) {
    cv::Mat frame16;
    cardFrame().convertTo(frame16, CV_16U, 257.0);

    const auto outline = lab_core::documentOutline(frame16);

    ASSERT_TRUE(outline);
    EXPECT_TRUE(findsTheCorners(*outline, 1, 4.0));
}

using Outline = std::vector<cv::Point2d>;

TEST(OutlineHold, KeepsTheLastOutlineForTenFramesThenLetsGo) {
    lab_core::OutlineHold hold;
    const Outline card{{1, 1}, {9, 1}, {9, 6}, {1, 6}};

    EXPECT_EQ(hold.next(card), card);
    for (int frame = 1; frame <= lab_core::kOutlineHoldFrames; ++frame) {
        EXPECT_EQ(hold.next(std::nullopt), card) << frame;
    }

    EXPECT_EQ(hold.next(std::nullopt), std::nullopt);
}

TEST(OutlineHold, ANewOutlineReplacesTheHeldOne) {
    lab_core::OutlineHold hold;
    const Outline first{{1, 1}, {9, 1}, {9, 6}, {1, 6}};
    const Outline second{{2, 2}, {8, 2}, {8, 5}, {2, 5}};
    hold.next(first);
    hold.next(std::nullopt);

    EXPECT_EQ(hold.next(second), second);
    EXPECT_EQ(hold.next(std::nullopt), second);
}

TEST(OutlineHold, NothingFoundYetHoldsNothing) {
    lab_core::OutlineHold hold;

    EXPECT_EQ(hold.next(std::nullopt), std::nullopt);
}

} // namespace
