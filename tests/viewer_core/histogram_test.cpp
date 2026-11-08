#include "viewer_core/histogram.h"

#include "viewer_core/display_converter.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using viewer_core::autoRange;
using viewer_core::DisplayRange;
using viewer_core::DisplaySettings;
using viewer_core::ImageTags;
using viewer_core::RangeMarkers;
using viewer_core::rangeMarkers;
using viewer_core::RangeMode;
using viewer_core::ValueExtent;
using viewer_core::valueExtent;

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();

cv::Mat filled(int rows, int cols, int type, const std::vector<double>& values) {
    cv::Mat image(rows, cols, type);
    cv::Mat flat = image.reshape(1, 1);
    for (int i = 0; i < flat.cols; ++i) {
        flat.col(i).setTo(values[static_cast<std::size_t>(i)]);
    }
    return image;
}

const ImageTags kHlgTenBit{.valueRange = {4100, 60218},
                           .transfer = viewer_core::Transfer::Hlg,
                           .primaries = viewer_core::Primaries::Bt2020};
const ImageTags kHlgFull{.valueRange = {0, 65535},
                         .transfer = viewer_core::Transfer::Hlg,
                         .primaries = viewer_core::Primaries::Bt2020};

// --- valueExtent -------------------------------------------------------------------------------------------

TEST(ValueExtent, IntegerIsTheWholeFramesMinAndMax) {
    cv::Mat image(64, 64, CV_8UC1, cv::Scalar(10));
    image.at<uchar>(1, 1) = 200;
    image.at<uchar>(62, 63) = 3;

    const ValueExtent extent = valueExtent(image);

    EXPECT_EQ(extent.all, (DisplayRange{3, 200}));
    EXPECT_EQ(extent.colours, (DisplayRange{3, 200}));
    EXPECT_EQ(extent.nonFinite, 0u);
}

TEST(ValueExtent, FloatLeavesOutAndCountsNonFinite) {
    const ValueExtent extent = valueExtent(filled(1, 5, CV_32FC1, {1, kNan, -2, kInfinity, -kInfinity}));

    EXPECT_EQ(extent.colours, (DisplayRange{-2, 1}));
    EXPECT_EQ(extent.all, (DisplayRange{-2, 1}));
    EXPECT_EQ(extent.nonFinite, 3u);
}

TEST(ValueExtent, NothingFinite) {
    const ValueExtent extent = valueExtent(cv::Mat(2, 3, CV_64FC1, cv::Scalar(kNan)));

    EXPECT_FALSE(extent.all);
    EXPECT_FALSE(extent.colours);
    EXPECT_EQ(extent.nonFinite, 6u);
}

TEST(ValueExtent, AlphaInAllButNotInColours) {
    const cv::Mat image(4, 4, CV_16UC4, cv::Scalar(100, 100, 100, 65535));

    const ValueExtent extent = valueExtent(image);

    EXPECT_EQ(extent.all, (DisplayRange{100, 65535}));
    EXPECT_EQ(extent.colours, (DisplayRange{100, 100}));
    ASSERT_TRUE(extent.all);
    EXPECT_EQ(*extent.all, autoRange(image));
}

// Large enough to be split across threads, so the pieces' results must combine to the masked pass's.
TEST(ValueExtent, LargeFloatFrameMatchesAutoRange) {
    cv::Mat image(1080, 1920, CV_32FC1);
    cv::randn(image, 0.0, 1.0);
    cv::Mat flat = image.reshape(1, 1);
    std::size_t planted = 0;
    for (int i = 0; i < flat.cols; i += 997) {
        flat.at<float>(0, i) = std::numeric_limits<float>::quiet_NaN();
        ++planted;
    }
    flat.at<float>(0, 5) = std::numeric_limits<float>::infinity();
    ++planted;

    const ValueExtent extent = valueExtent(image);

    ASSERT_TRUE(extent.all);
    EXPECT_EQ(*extent.all, autoRange(image));
    EXPECT_EQ(extent.colours, extent.all);
    EXPECT_EQ(extent.nonFinite, planted);
}

TEST(ValueExtent, RejectsUnsupported) {
    EXPECT_THROW(valueExtent(cv::Mat()), std::invalid_argument);
    EXPECT_THROW(valueExtent(cv::Mat(2, 2, CV_8UC(5))), std::invalid_argument);
}

// --- rangeMarkers ------------------------------------------------------------------------------------------

TEST(RangeMarkers, AutoEightBitIsTheFullScale) {
    EXPECT_EQ(rangeMarkers(CV_8U, 1, DisplaySettings{}, {}, {.all = DisplayRange{3, 200}}),
              (RangeMarkers{.range = {0, 255}}));
}

TEST(RangeMarkers, AutoOtherDepthsAreAutoRange) {
    const auto markers = rangeMarkers(CV_16U, 1, DisplaySettings{}, {}, {.all = DisplayRange{100, 900}});

    ASSERT_TRUE(markers);
    EXPECT_EQ(markers->range, (DisplayRange{100, 900}));
    EXPECT_FALSE(markers->scaleTop);
    EXPECT_FALSE(markers->hdr);
}

TEST(RangeMarkers, AutoWithAHintIsTheHint) {
    const auto markers = rangeMarkers(CV_16U, 1, DisplaySettings{}, {.valueRange = {0, 1023}},
                                      {.all = DisplayRange{100, 900}});

    ASSERT_TRUE(markers);
    EXPECT_EQ(markers->range, (DisplayRange{0, 1023}));
}

TEST(RangeMarkers, HoldAndFixedAreTheirOwn) {
    for (const RangeMode mode : {RangeMode::Fixed, RangeMode::Hold}) {
        const auto markers =
            rangeMarkers(CV_16U, 1, {.mode = mode, .range = {5, 50}}, {}, {.all = DisplayRange{100, 900}});

        ASSERT_TRUE(markers);
        EXPECT_EQ(markers->range, (DisplayRange{5, 50}));
        EXPECT_FALSE(markers->hdr);
    }
}

TEST(RangeMarkers, HdrIsBlackWhiteAndTop) {
    const auto markers = rangeMarkers(CV_16U, 3, {.mode = RangeMode::Hdr}, kHlgTenBit, {});

    ASSERT_TRUE(markers);
    EXPECT_EQ(markers->range, (DisplayRange{4100, 60218}));
    EXPECT_EQ(markers->scaleTop, 65535.0);
    EXPECT_TRUE(markers->hdr);
}

TEST(RangeMarkers, HdrFloatTopIsWhite) {
    const auto markers =
        rangeMarkers(CV_32F, 3, {.mode = RangeMode::Hdr}, {.transfer = viewer_core::Transfer::Pq}, {});

    ASSERT_TRUE(markers);
    EXPECT_EQ(markers->range, (DisplayRange{0, 1}));
    EXPECT_EQ(markers->scaleTop, 1.0);
    EXPECT_TRUE(markers->hdr);
}

TEST(RangeMarkers, HdrOverrideIsTheRange) {
    const auto markers =
        rangeMarkers(CV_16U, 3, {.mode = RangeMode::Hdr, .range = {2000, 30000}}, kHlgFull, {});

    ASSERT_TRUE(markers);
    EXPECT_EQ(markers->range, (DisplayRange{2000, 30000}));
    EXPECT_EQ(markers->scaleTop, 65535.0);
    EXPECT_TRUE(markers->hdr);
}

TEST(RangeMarkers, HdrOnUntaggedOrTwoChannelsIsAuto) {
    const auto untagged =
        rangeMarkers(CV_16U, 1, {.mode = RangeMode::Hdr}, {}, {.all = DisplayRange{100, 900}});
    ASSERT_TRUE(untagged);
    EXPECT_EQ(untagged->range, (DisplayRange{100, 900}));
    EXPECT_FALSE(untagged->hdr);

    const auto twoChannels = rangeMarkers(CV_16U, 2, {.mode = RangeMode::Hdr}, kHlgFull, {});
    ASSERT_TRUE(twoChannels);
    EXPECT_EQ(twoChannels->range, (DisplayRange{0, 65535}));
    EXPECT_FALSE(twoChannels->hdr);
    EXPECT_FALSE(twoChannels->scaleTop);
}

TEST(RangeMarkers, NothingFiniteHasNone) {
    EXPECT_FALSE(rangeMarkers(CV_32F, 1, DisplaySettings{}, {}, ValueExtent{}));
}

// --- histogramOf -------------------------------------------------------------------------------------------

using viewer_core::axisFraction;
using viewer_core::axisValue;
using viewer_core::draggedRange;
using viewer_core::Histogram;
using viewer_core::histogramOf;
using viewer_core::kHistogramSamples;
using viewer_core::RangeHandle;
using viewer_core::settingsForDrag;

// With Auto's markers, as the viewer shows an image by default.
Histogram autoHistogram(const cv::Mat& image, std::size_t cap = kHistogramSamples) {
    const ValueExtent extent = valueExtent(image);
    return histogramOf(image, extent, rangeMarkers(image.depth(), image.channels(), {}, {}, extent), cap);
}

Histogram markedHistogram(const cv::Mat& image, const RangeMarkers& markers,
                          std::size_t cap = kHistogramSamples) {
    return histogramOf(image, valueExtent(image), markers, cap);
}

std::uint64_t total(const std::vector<std::uint32_t>& counts) {
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
}

// Every value 0-255 once.
cv::Mat eightBitRamp() {
    cv::Mat image(16, 16, CV_8UC1);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            image.at<uchar>(y, x) = static_cast<uchar>(16 * y + x);
        }
    }
    return image;
}

// 1000 to 1099: fewer codes than bins.
cv::Mat hundredCodes() {
    cv::Mat image(1, 100, CV_16UC1);
    for (int x = 0; x < 100; ++x) {
        image.at<std::uint16_t>(0, x) = static_cast<std::uint16_t>(1000 + x);
    }
    return image;
}

cv::Mat floatRamp() {
    cv::Mat image(1, 101, CV_32FC1);
    for (int x = 0; x < 101; ++x) {
        image.at<float>(0, x) = static_cast<float>(x) * 0.01F;
    }
    return image;
}

TEST(Histogram, EightBitCountsEveryValue) {
    const Histogram histogram = autoHistogram(eightBitRamp());

    EXPECT_EQ(histogram.axis, (DisplayRange{0, 256}));
    EXPECT_EQ(histogram.values, (DisplayRange{0, 255}));
    EXPECT_EQ(histogram.binWidth, 1.0);
    ASSERT_EQ(histogram.counts.size(), 1u);
    ASSERT_EQ(histogram.counts[0].size(), 256u);
    for (std::size_t value = 0; value < 256; ++value) {
        EXPECT_EQ(histogram.counts[0][value], 1u) << value;
    }
    EXPECT_EQ(histogram.step, 1);
    EXPECT_EQ(histogram.samples, 256u);
}

TEST(Histogram, ChannelsInOpenCVsOrderAlphaLeftOut) {
    const Histogram histogram = autoHistogram(cv::Mat(4, 4, CV_8UC4, cv::Scalar(10, 20, 30, 255)));

    ASSERT_EQ(histogram.counts.size(), 3u);
    EXPECT_EQ(histogram.counts[0][10], 16u);
    EXPECT_EQ(histogram.counts[1][20], 16u);
    EXPECT_EQ(histogram.counts[2][30], 16u);
}

TEST(Histogram, TwoChannelsGiveTwoCurves) {
    EXPECT_EQ(autoHistogram(cv::Mat(4, 4, CV_8UC2, cv::Scalar(1, 2))).counts.size(), 2u);
}

TEST(Histogram, AlphaDoesNotWidenTheAxis) {
    const Histogram histogram =
        markedHistogram(cv::Mat(4, 4, CV_16UC4, cv::Scalar(1000, 1000, 1000, 65535)), {.range = {900, 1100}});

    EXPECT_LT(histogram.values.max, 2000.0);
}

TEST(Histogram, FewCodesGetOneBinEach) {
    const Histogram histogram = autoHistogram(hundredCodes());

    EXPECT_EQ(histogram.binWidth, 1.0);
    EXPECT_EQ(histogram.axis, (DisplayRange{993, 1107})); // 1000-1099 and 99/16 each side, in whole codes
    ASSERT_EQ(histogram.counts.size(), 1u);
    for (std::size_t code = 1000; code < 1100; ++code) {
        EXPECT_EQ(histogram.counts[0][code - 993], 1u) << code;
    }
    EXPECT_EQ(total(histogram.counts[0]), 100u);
}

TEST(Histogram, WideIntegerBinsSpanWholeCodes) {
    const Histogram histogram = autoHistogram(filled(1, 2, CV_16UC1, {0, 65535}));

    EXPECT_EQ(histogram.binWidth, 256.0);
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(histogram.counts[0].size(), 256u);
    EXPECT_EQ(histogram.axis, (DisplayRange{0, 65536}));
    EXPECT_EQ(histogram.values, (DisplayRange{0, 65535}));
}

TEST(Histogram, FullInt32RangeStaysWhole) {
    constexpr double kLowest = std::numeric_limits<std::int32_t>::min();
    constexpr double kHighest = std::numeric_limits<std::int32_t>::max();

    const Histogram histogram = autoHistogram(filled(1, 2, CV_32SC1, {kLowest, kHighest}));

    EXPECT_EQ(histogram.binWidth, 16777216.0); // 2^32 codes in 256 bins
    ASSERT_EQ(histogram.counts.size(), 1u);
    ASSERT_EQ(histogram.counts[0].size(), 256u);
    EXPECT_EQ(histogram.values, (DisplayRange{kLowest, kHighest}));
    EXPECT_EQ(histogram.counts[0].front(), 1u);
    EXPECT_EQ(histogram.counts[0].back(), 1u);
}

TEST(Histogram, FloatsUse256Bins) {
    const Histogram histogram = autoHistogram(floatRamp());

    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(histogram.counts[0].size(), 256u);
    EXPECT_NEAR(histogram.binWidth, (histogram.axis.max - histogram.axis.min) / 256, 1e-12);
    EXPECT_EQ(total(histogram.counts[0]), 101u);
}

TEST(Histogram, AxisIsDataAndRangeWithAMargin) {
    const Histogram histogram = markedHistogram(filled(1, 2, CV_32FC1, {0, 10}), {.range = {-10, 5}});

    EXPECT_EQ(histogram.axis, (DisplayRange{-11.25, 11.25}));
    EXPECT_EQ(histogram.values, histogram.axis);
}

TEST(Histogram, FlatImageGetsOneUnit) {
    const Histogram histogram = autoHistogram(cv::Mat(3, 3, CV_32FC1, cv::Scalar(7)));

    EXPECT_EQ(histogram.axis, (DisplayRange{6.4375, 7.5625}));
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(*std::max_element(histogram.counts[0].begin(), histogram.counts[0].end()), 9u);
}

TEST(Histogram, HdrAxisReachesTheScaleTop) {
    const Histogram histogram = markedHistogram(cv::Mat(2, 2, CV_16UC3, cv::Scalar::all(30000)),
                                                {.range = {4100, 60218}, .scaleTop = 65535, .hdr = true});

    // 4100 less 61435/16 is 260.3; 65276 codes from 260, in 256 bins of 255.
    EXPECT_EQ(histogram.values, (DisplayRange{260, 65535}));
    EXPECT_EQ(histogram.axis, (DisplayRange{260, 65540}));
    EXPECT_EQ(histogram.binWidth, 255.0);
    EXPECT_EQ(axisValue(histogram, 1.0), 65535.0);
}

TEST(Histogram, MarkersBeyondTheDepthStayOnTheAxis) {
    const Histogram histogram =
        markedHistogram(cv::Mat(2, 2, CV_8UC1, cv::Scalar(100)), {.range = {-100, 300}});

    EXPECT_EQ(histogram.values, (DisplayRange{-100, 300}));
}

TEST(Histogram, SamplesAtMostTheCap) {
    const Histogram histogram = autoHistogram(cv::Mat(100, 100, CV_8UC1, cv::Scalar(5)), 1000);

    EXPECT_EQ(histogram.step, 4); // every 3rd would take 34 × 34
    EXPECT_EQ(histogram.samples, 625u);
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(total(histogram.counts[0]), 625u);
}

TEST(Histogram, BelowTheCapCountsEveryPixel) {
    const Histogram histogram = autoHistogram(cv::Mat(30, 30, CV_8UC1, cv::Scalar(5)), 1000);

    EXPECT_EQ(histogram.step, 1);
    EXPECT_EQ(histogram.samples, 900u);
}

TEST(Histogram, SamplesOnAnEvenGrid) {
    cv::Mat image(8, 8, CV_8UC1, cv::Scalar(0));
    for (int x = 1; x < 8; x += 2) {
        image.col(x).setTo(200);
    }

    const Histogram histogram = autoHistogram(image, 16);

    EXPECT_EQ(histogram.step, 2);
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(histogram.counts[0][200], 0u);
    EXPECT_EQ(histogram.counts[0][0], 16u);
}

TEST(Histogram, ExtentFromTheWholeFrameNotTheSamples) {
    cv::Mat image(8, 8, CV_16UC1, cv::Scalar(100));
    image.at<std::uint16_t>(1, 1) = 5000; // off the grid of every 2nd pixel

    const Histogram histogram = autoHistogram(image, 16);

    EXPECT_GT(histogram.values.max, 5000.0);
    ASSERT_EQ(histogram.counts.size(), 1u);
    std::uint64_t atOrAbove = 0;
    for (std::size_t bin = 0; bin < histogram.counts[0].size(); ++bin) {
        if (histogram.axis.min + static_cast<double>(bin) * histogram.binWidth >= 5000.0) {
            atOrAbove += histogram.counts[0][bin];
        }
    }
    EXPECT_EQ(atOrAbove, 0u);
}

TEST(Histogram, NonFiniteCountIsTheWholeFrames) {
    cv::Mat image(8, 8, CV_32FC1, cv::Scalar(1));
    image.at<float>(1, 1) = std::numeric_limits<float>::quiet_NaN(); // off the grid

    const Histogram histogram = autoHistogram(image, 16);

    EXPECT_EQ(histogram.nonFinite, 1u);
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(total(histogram.counts[0]), 16u);
}

TEST(Histogram, NothingFiniteHasNoCurves) {
    const cv::Mat image(8, 8, CV_32FC1, cv::Scalar(kNan));

    const Histogram histogram = histogramOf(image, valueExtent(image), std::nullopt);

    EXPECT_TRUE(histogram.counts.empty());
    EXPECT_EQ(histogram.nonFinite, 64u);
    EXPECT_EQ(histogram.axis, (DisplayRange{0, 1}));
}

TEST(Histogram, HugeFloatRangeStaysFinite) {
    const Histogram histogram = autoHistogram(filled(1, 2, CV_64FC1, {-1e308, 1e308}));

    EXPECT_TRUE(std::isfinite(histogram.axis.min));
    EXPECT_TRUE(std::isfinite(histogram.axis.max));
    EXPECT_TRUE(std::isfinite(histogram.binWidth));
    EXPECT_NEAR(axisFraction(histogram, 0.0), 0.5, 1e-12);
    const double middle = axisValue(histogram, 0.5);
    EXPECT_TRUE(std::isfinite(middle));
    EXPECT_LE(std::abs(middle), 1e-12 * histogram.axis.max);
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(total(histogram.counts[0]), 2u);
}

TEST(Histogram, OnePixelOddSizesAndZeroCap) {
    EXPECT_EQ(autoHistogram(cv::Mat(1, 1, CV_8UC1, cv::Scalar(7))).samples, 1u);

    const Histogram odd = autoHistogram(cv::Mat(3, 7, CV_16UC3, cv::Scalar::all(9)), 4);
    EXPECT_EQ(odd.step, 3); // row 0; columns 0, 3 and 6
    EXPECT_EQ(odd.samples, 3u);

    EXPECT_EQ(autoHistogram(cv::Mat(4, 4, CV_8UC1, cv::Scalar(7)), 0).samples, 1u);
}

// A Fixed range typed as -1e308 to 1e308, or a sender's value range as wide, on whole numbers: more codes
// than a double holds. The bins are laid out as for floating point, and nothing overflows.
TEST(Histogram, IntegerImageWithARangeBeyondEveryDoubleStaysSafe) {
    const Histogram fixed =
        markedHistogram(cv::Mat(2, 2, CV_8UC1, cv::Scalar(100)), {.range = {-1e308, 1e308}});
    EXPECT_TRUE(std::isfinite(fixed.axis.min));
    EXPECT_TRUE(std::isfinite(fixed.axis.max));
    EXPECT_TRUE(std::isfinite(fixed.binWidth));
    ASSERT_EQ(fixed.counts.size(), 1u);
    EXPECT_GT(fixed.counts[0].size(), 0u);
    EXPECT_LE(fixed.counts[0].size(), 256u);
    EXPECT_EQ(total(fixed.counts[0]), 4u);
    const DisplayRange dragged = draggedRange(fixed, {-1e308, 1e308}, RangeHandle::Black, 0.0, 0.25, 1);
    EXPECT_TRUE(std::isfinite(dragged.min));
    EXPECT_LT(dragged.min, dragged.max);

    const Histogram hinted = markedHistogram(cv::Mat(2, 2, CV_16UC3, cv::Scalar::all(30000)),
                                             {.range = {-1e308, 1e308}, .scaleTop = 65535, .hdr = true});
    EXPECT_TRUE(std::isfinite(hinted.axis.max));
    ASSERT_EQ(hinted.counts.size(), 3u);
    for (const auto& counts : hinted.counts) {
        EXPECT_EQ(total(counts), 4u);
    }
}

// Two doubles so close that half their difference is 0: the axis must not collapse to one point.
TEST(Histogram, TinyFloatSpanCountsSafely) {
    const Histogram histogram =
        autoHistogram(filled(1, 2, CV_64FC1, {0, std::numeric_limits<double>::denorm_min()}));

    EXPECT_GT(histogram.axis.max / 2 - histogram.axis.min / 2, 0.0);
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_EQ(total(histogram.counts[0]), 2u);
}

// --- The axis ----------------------------------------------------------------------------------------------

TEST(HistogramAxis, FractionRoundTrip) {
    for (const Histogram& histogram :
         {autoHistogram(floatRamp()), autoHistogram(filled(1, 2, CV_64FC1, {-1e308, 1e308}))}) {
        const double halfSpan = histogram.values.max / 2 - histogram.values.min / 2;
        for (int k = 0; k <= 9; ++k) {
            const double value = 2 * (histogram.values.min / 2 + k / 9.0 * halfSpan);
            EXPECT_NEAR(axisValue(histogram, axisFraction(histogram, value)), value, 2e-12 * halfSpan) << k;
        }
    }
}

TEST(HistogramAxis, IntegerValuesSnapToCodes) {
    const Histogram histogram = autoHistogram(hundredCodes());

    EXPECT_EQ(axisValue(histogram, axisFraction(histogram, 1000.4)), 1000.0);
}

TEST(HistogramAxis, ValueClampedToValues) {
    const Histogram histogram = autoHistogram(eightBitRamp());

    EXPECT_EQ(axisValue(histogram, -1.0), 0.0);
    EXPECT_EQ(axisValue(histogram, 2.0), 255.0);
}

TEST(HistogramAxis, EmptyHistogramIsSafe) {
    const Histogram empty{};

    EXPECT_TRUE(std::isfinite(axisFraction(empty, 0.5)));
    EXPECT_TRUE(std::isfinite(axisValue(empty, 0.5)));
}

// --- Dragging ----------------------------------------------------------------------------------------------

// 0 to 100 in floating point, so a fraction of the axis is a hundredth of a value.
Histogram percentAxis() {
    return Histogram{.axis = {0, 100}, .values = {0, 100}, .depth = CV_32F, .binWidth = 100.0 / 256};
}

void expectRangeNear(const DisplayRange& actual, const DisplayRange& expected) {
    EXPECT_NEAR(actual.min, expected.min, 1e-9);
    EXPECT_NEAR(actual.max, expected.max, 1e-9);
}

TEST(DraggedRange, AnEndMovesByTheDrag) {
    expectRangeNear(draggedRange(percentAxis(), {10, 20}, RangeHandle::Black, 0.3, 0.35, 1), {15, 20});
}

TEST(DraggedRange, EndsNeverCross) {
    expectRangeNear(draggedRange(percentAxis(), {10, 20}, RangeHandle::Black, 0.3, 0.9, 1), {19, 20});
    expectRangeNear(draggedRange(percentAxis(), {10, 20}, RangeHandle::White, 0.2, -0.5, 1), {10, 11});
}

TEST(DraggedRange, BothKeepsTheWidth) {
    expectRangeNear(draggedRange(percentAxis(), {10, 20}, RangeHandle::Both, 0.5, 0.6, 1), {20, 30});
    expectRangeNear(draggedRange(percentAxis(), {10, 20}, RangeHandle::Both, 0.5, 5.0, 1), {90, 100});
}

TEST(DraggedRange, IntegerEndsSnap) {
    const Histogram histogram = autoHistogram(eightBitRamp());

    EXPECT_EQ(draggedRange(histogram, {0, 255}, RangeHandle::White, axisFraction(histogram, 255),
                           axisFraction(histogram, 200.6), 0.5),
              (DisplayRange{0, 201}));
}

TEST(DraggedRange, IntegerMiddleSnapsTheDistance) {
    const Histogram histogram = autoHistogram(eightBitRamp());

    EXPECT_EQ(draggedRange(histogram, {10, 20}, RangeHandle::Both, axisFraction(histogram, 50),
                           axisFraction(histogram, 55.4), 0.5),
              (DisplayRange{15, 25}));
}

TEST(DraggedRange, EndsStayApartAtTheDepthLimit) {
    const Histogram histogram = autoHistogram(eightBitRamp());

    EXPECT_EQ(draggedRange(histogram, {0, 255}, RangeHandle::White, axisFraction(histogram, 255), -1.0, 0.1),
              (DisplayRange{0, 1}));
    EXPECT_EQ(draggedRange(histogram, {0, 255}, RangeHandle::Black, axisFraction(histogram, 0), 2.0, 0.1),
              (DisplayRange{254, 255}));
}

TEST(DraggedRange, FromARangeBeyondTheDepth) {
    const Histogram histogram =
        markedHistogram(cv::Mat(2, 2, CV_8UC1, cv::Scalar(100)), {.range = {-100, 300}});

    // The range already spans everything a drag can reach, so the middle cannot move.
    EXPECT_EQ(draggedRange(histogram, {-100, 300}, RangeHandle::Both, 0.5, 0.6, 0.5),
              (DisplayRange{-100, 300}));
    // 401 codes in 201 bins of 2: the axis spans 402, so half of it moves black by 201.
    EXPECT_EQ(
        draggedRange(histogram, {-100, 300}, RangeHandle::Black, axisFraction(histogram, -100), 0.5, 0.5),
        (DisplayRange{101, 300}));
}

TEST(SettingsForDrag, FixedKeepsTheColours) {
    EXPECT_EQ(
        settingsForDrag({.mode = RangeMode::Auto, .colours = viewer_core::ColourMap::Turbo}, false, {1, 2}),
        (DisplaySettings{
            .mode = RangeMode::Fixed, .range = {1, 2}, .colours = viewer_core::ColourMap::Turbo}));
}

TEST(SettingsForDrag, HdrStaysHdr) {
    EXPECT_EQ(settingsForDrag({.mode = RangeMode::Hdr}, true, {100, 900}),
              (DisplaySettings{.mode = RangeMode::Hdr, .range = {100, 900}}));
}

// --- Cost
// ----------------------------------------------------------------------------------------------------

template <typename Work> double medianMilliseconds(Work work, int runs = 40) {
    std::vector<double> times;
    work(); // warm up
    for (int run = 0; run < runs; ++run) {
        const auto start = std::chrono::steady_clock::now();
        work();
        times.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// What the histogram of a 4K frame costs per frame, extent included, at each sample cap: the measurement
// kHistogramSamples was chosen from. Run by hand in a release build:
// --gtest_also_run_disabled_tests --gtest_filter=*HistogramMeasurement*
TEST(HistogramMeasurement, DISABLED_Cost) {
    cv::RNG random(7);
    const cv::Size size(3840, 2160);
    cv::Mat sixteen(size, CV_16UC3);
    random.fill(sixteen, cv::RNG::UNIFORM, 0, 65536);
    cv::Mat eight(size, CV_8UC3);
    random.fill(eight, cv::RNG::UNIFORM, 0, 256);
    cv::Mat floats(size, CV_32FC1);
    random.fill(floats, cv::RNG::NORMAL, 0.0, 1.0);
    cv::Mat withNan = floats.clone();
    cv::Mat flat = withNan.reshape(1, 1);
    for (int i = 0; i < flat.cols; i += 100) {
        flat.at<float>(0, i) = std::numeric_limits<float>::quiet_NaN(); // 1 %
    }
    const std::vector<std::pair<const char*, const cv::Mat*>> frames{
        {"16UC3", &sixteen}, {"8UC3", &eight}, {"32FC1", &floats}, {"32FC1, 1 % NaN", &withNan}};

    for (const auto& [name, frame] : frames) {
        const double extentOnly = medianMilliseconds([&] { (void)valueExtent(*frame); });
        std::cout << "[ histogram ] " << name << " 4K: extent " << extentOnly << " ms\n";
        for (const std::size_t cap :
             {std::size_t{1} << 17, std::size_t{1} << 18, std::size_t{1} << 19, std::size_t{1} << 20}) {
            const double whole = medianMilliseconds([&] {
                const ValueExtent extent = valueExtent(*frame);
                const auto markers = rangeMarkers(frame->depth(), frame->channels(), {}, {}, extent);
                (void)histogramOf(*frame, extent, markers, cap);
            });
            std::cout << "[ histogram ] " << name << " 4K, cap " << cap << ": " << whole << " ms\n";
        }
    }
}

} // namespace
