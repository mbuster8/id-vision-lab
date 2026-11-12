#include "doc_vision/page_geometry.h"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

using doc_vision::Quad;

constexpr double kDegree = std::numbers::pi / 180.0;

cv::Point2d rotated(cv::Point2d p, double degrees, cv::Point2d about) {
    const double c = std::cos(degrees * kDegree);
    const double s = std::sin(degrees * kDegree);
    return {about.x + p.x * c - p.y * s, about.y + p.x * s + p.y * c};
}

// A w × h rectangle centred at `about`, turned by `degrees` (clockwise on screen), corners in its own TL, TR,
// BR, BL order.
std::array<cv::Point2d, 4> turnedRectangle(double w, double h, double degrees, cv::Point2d about) {
    return {rotated({-w / 2, -h / 2}, degrees, about), rotated({w / 2, -h / 2}, degrees, about),
            rotated({w / 2, h / 2}, degrees, about), rotated({-w / 2, h / 2}, degrees, about)};
}

void expectNear(cv::Point2d actual, cv::Point2d expected, double tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
}

TEST(PageGeometry, OrdersAnyPermutation) {
    const std::array<cv::Point2d, 4> corners{{{100, 100}, {500, 100}, {500, 380}, {100, 380}}};
    std::array<int, 4> order{0, 1, 2, 3};
    do {
        const Quad quad = doc_vision::orderedCorners(
            {corners[order[0]], corners[order[1]], corners[order[2]], corners[order[3]]});
        for (int i = 0; i < 4; ++i) {
            expectNear(quad[i], corners[i], 1e-9);
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(PageGeometry, PortraitStartsOnTheLeftLongSide) {
    const Quad quad = doc_vision::orderedCorners({{{100, 100}, {380, 100}, {380, 500}, {100, 500}}});
    expectNear(quad[0], {100, 500}, 1e-9);
    expectNear(quad[1], {100, 100}, 1e-9);
    expectNear(quad[2], {380, 100}, 1e-9);
    expectNear(quad[3], {380, 500}, 1e-9);
}

TEST(PageGeometry, FortyFiveDegreesKeepsItsStartThroughJitter) {
    for (const double degrees : {43.0, 45.0, 47.0}) {
        const std::array<cv::Point2d, 4> page = turnedRectangle(400, 282, degrees, {640, 360});
        const Quad quad = doc_vision::orderedCorners(page);
        expectNear(quad[0], page[0], 1e-9);
    }
}

TEST(PageGeometry, AlignToKeepsAPortraitStart) {
    // The TD3 landscape page turned about 90°: which long side is higher flips between 88° and 92°.
    const Quad at88 = doc_vision::orderedCorners(turnedRectangle(400, 282, 88, {640, 360}));
    const Quad at92 = doc_vision::orderedCorners(turnedRectangle(400, 282, 92, {640, 360}));
    ASSERT_GT(cv::norm(at88[0] - at92[0]), 300.0) << "the flip the alignment exists for";
    const Quad aligned = doc_vision::alignTo(at92, at88);
    EXPECT_LT(cv::norm(aligned[0] - at88[0]), 20.0);
}

TEST(PageGeometry, AlignToNeverShiftsByOne) {
    // Turned 85°, the corner nearest the upright page's TL is one along a short side: a shift of 1 or 3 would
    // be closest, and would start on a short side.
    const Quad upright = doc_vision::orderedCorners(turnedRectangle(85, 60, 0, {200, 200}));
    const Quad turned = doc_vision::orderedCorners(turnedRectangle(85, 60, 85, {200, 200}));
    const Quad aligned = doc_vision::alignTo(turned, upright);
    EXPECT_NEAR(cv::norm(aligned[1] - aligned[0]), 85.0, 1e-6);
}

TEST(PageGeometry, FlattenMapsTheQuadOntoThePage) {
    cv::Mat page(doc_vision::kPageSize, CV_8UC3, cv::Scalar::all(180));
    cv::rectangle(page, cv::Rect(0, 0, 40, 40), cv::Scalar(0, 0, 255), cv::FILLED);
    const Quad quad{{{200, 100}, {1000, 140}, {980, 640}, {220, 600}}};
    const std::vector<cv::Point2f> pageCorners{{0, 0}, {1249, 0}, {1249, 879}, {0, 879}};
    const std::vector<cv::Point2f> frameCorners(quad.begin(), quad.end());
    cv::Mat frame(720, 1280, CV_8UC3, cv::Scalar(50, 40, 45));
    cv::warpPerspective(page, frame, cv::getPerspectiveTransform(pageCorners, frameCorners), frame.size(),
                        cv::INTER_LINEAR, cv::BORDER_TRANSPARENT);

    const cv::Mat flat = doc_vision::flatten(frame, quad, doc_vision::kPageSize);
    ASSERT_EQ(flat.size(), doc_vision::kPageSize);
    const cv::Vec3b corner = flat.at<cv::Vec3b>(10, 10);
    EXPECT_LT(corner[0], 60);
    EXPECT_LT(corner[1], 60);
    EXPECT_GT(corner[2], 200);
    const cv::Vec3b far = flat.at<cv::Vec3b>(870, 1240);
    EXPECT_FALSE(far[0] < 60 && far[1] < 60 && far[2] > 200);
}

// A4: the MRZ band found on the flattened page, drawn back on the frame; flatten's own correspondence.
TEST(PageGeometry, PageToFrameMapsThePagesCornersOntoTheQuad) {
    const Quad quad{{{200, 100}, {1000, 140}, {980, 640}, {220, 600}}};
    const Quad pageCorners{{{0, 0}, {1249, 0}, {1249, 879}, {0, 879}}};
    const Quad mapped = doc_vision::pageToFrame(pageCorners, quad, doc_vision::kPageSize);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(mapped[i].x, quad[i].x, 1e-6) << i;
        EXPECT_NEAR(mapped[i].y, quad[i].y, 1e-6) << i;
    }
    // A point and its flattened twin: a marker drawn on the page lands where pageToFrame says.
    const Quad marker{{{600, 400}, {600, 400}, {600, 400}, {600, 400}}};
    const cv::Point2d onFrame = doc_vision::pageToFrame(marker, quad, doc_vision::kPageSize)[0];
    cv::Mat page(doc_vision::kPageSize, CV_8UC1, cv::Scalar::all(0));
    cv::circle(page, cv::Point(600, 400), 6, cv::Scalar::all(255), cv::FILLED);
    const std::vector<cv::Point2f> from{{0, 0}, {1249, 0}, {1249, 879}, {0, 879}};
    const std::vector<cv::Point2f> to(quad.begin(), quad.end());
    cv::Mat frame;
    cv::warpPerspective(page, frame, cv::getPerspectiveTransform(from, to), cv::Size(1280, 720));
    const cv::Moments m = cv::moments(frame, true);
    ASSERT_GT(m.m00, 0);
    EXPECT_NEAR(m.m10 / m.m00, onFrame.x, 1.0);
    EXPECT_NEAR(m.m01 / m.m00, onFrame.y, 1.0);
}

TEST(PageGeometry, UnturnedPutsAQuadBackTheRightWayUp) {
    const Quad onTurned{{{60, 30}, {1190, 30}, {1190, 120}, {60, 120}}};
    const Quad back = doc_vision::unturned(onTurned, doc_vision::kPageSize);
    EXPECT_EQ(back[0], cv::Point2d(1249 - 60, 879 - 30));
    EXPECT_EQ(back[2], cv::Point2d(1249 - 1190, 879 - 120));
}

TEST(PageGeometry, FlattenIsAnIdentityOnItsOwnCorners) {
    cv::Mat frame(doc_vision::kGatePageSize, CV_8UC3);
    cv::RNG rng(7);
    rng.fill(frame, cv::RNG::UNIFORM, 0, 256);
    const Quad quad{{{0, 0}, {624, 0}, {624, 439}, {0, 439}}};
    const cv::Mat flat = doc_vision::flatten(frame, quad, doc_vision::kGatePageSize);
    cv::Mat difference;
    cv::absdiff(flat, frame, difference);
    double largest = 0;
    cv::minMaxLoc(difference.reshape(1), nullptr, &largest);
    EXPECT_LE(largest, 1.0);
}

TEST(PageGeometry, Shares) {
    const cv::Size frame(1280, 720);
    EXPECT_DOUBLE_EQ(doc_vision::fillShare({{{0, 0}, {1280, 0}, {1280, 720}, {0, 720}}}, frame), 1.0);
    EXPECT_DOUBLE_EQ(doc_vision::fillShare({{{0, 0}, {640, 0}, {640, 360}, {0, 360}}}, frame), 0.25);
    EXPECT_DOUBLE_EQ(doc_vision::longerSideShare({{{100, 100}, {612, 100}, {612, 400}, {100, 400}}}, frame),
                     0.4);
    EXPECT_DOUBLE_EQ(doc_vision::extentShare({{{40, 60}, {1240, 60}, {1240, 660}, {40, 660}}}, frame),
                     0.9375);
    EXPECT_NEAR(doc_vision::edgeMarginShare({{{10, 100}, {900, 100}, {900, 600}, {30, 600}}}, frame),
                10.0 / 720.0, 1e-9);
}

// The TD3 page projected through a camera of 70° horizontal field of view at 400 mm, turned by `degrees`
// about the page's horizontal (x) or vertical (y) axis.
Quad projectedPage(double degrees, bool aboutX) {
    const double f = 640.0 / std::tan(35.0 * kDegree);
    const double c = std::cos(degrees * kDegree);
    const double s = std::sin(degrees * kDegree);
    std::array<cv::Point2d, 4> image;
    const std::array<cv::Point2d, 4> plane{{{-62.5, -44}, {62.5, -44}, {62.5, 44}, {-62.5, 44}}};
    for (int i = 0; i < 4; ++i) {
        const double x = plane[i].x;
        const double y = plane[i].y;
        // Turned about the page's centre, then 400 mm in front of the camera.
        const double X = aboutX ? x : x * c;
        const double Y = aboutX ? y * c : y;
        const double Z = 400.0 + (aboutX ? y * s : -x * s);
        image[i] = {f * X / Z + 640.0, f * Y / Z + 360.0};
    }
    return doc_vision::orderedCorners(image);
}

TEST(PageGeometry, TiltOfAFrontoParallelPageIsZero) {
    EXPECT_NEAR(doc_vision::tiltDegrees(projectedPage(0, true), {1280, 720}), 0.0, 0.5);
}

TEST(PageGeometry, TiltFollowsRotation) {
    for (const bool aboutX : {true, false}) {
        for (const double degrees : {10.0, 20.0, 30.0}) {
            EXPECT_NEAR(doc_vision::tiltDegrees(projectedPage(degrees, aboutX), {1280, 720}), degrees, 1.0)
                << (aboutX ? "about x, " : "about y, ") << degrees << "°";
        }
    }
}

} // namespace
