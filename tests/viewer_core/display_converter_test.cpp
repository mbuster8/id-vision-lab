#include "viewer_core/display_converter.h"
#include "viewer_core/hdr_tone_map.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using viewer_core::DisplaySettings;
using viewer_core::RangeMode;
using viewer_core::sizeWithin;
using viewer_core::toDisplayImage;

cv::Vec4b displayPixel(const cv::Mat& image, int row = 0, int col = 0) {
    const cv::Mat display = toDisplayImage(image);
    EXPECT_EQ(display.type(), CV_8UC4);
    return display.at<cv::Vec4b>(row, col);
}

TEST(DisplayConverter, EightBitGreyBecomesOpaqueGrey) {
    const cv::Mat image(1, 1, CV_8UC1, cv::Scalar(37));

    EXPECT_EQ(displayPixel(image), cv::Vec4b(37, 37, 37, 255));
}

TEST(DisplayConverter, EightBitBgrBecomesRgba) {
    const cv::Mat image(1, 1, CV_8UC3, cv::Scalar(10, 20, 30));

    EXPECT_EQ(displayPixel(image), cv::Vec4b(30, 20, 10, 255));
}

TEST(DisplayConverter, EightBitBgraBecomesRgba) {
    const cv::Mat image(1, 1, CV_8UC4, cv::Scalar(10, 20, 30, 40));

    EXPECT_EQ(displayPixel(image), cv::Vec4b(30, 20, 10, 40));
}

TEST(DisplayConverter, TwoChannelsMapToRedAndGreen) {
    const cv::Mat image(1, 1, CV_8UC2, cv::Scalar(5, 6));

    EXPECT_EQ(displayPixel(image), cv::Vec4b(5, 6, 0, 255));
}

TEST(DisplayConverter, NonEightBitIsNormalisedAcrossAllChannels) {
    cv::Mat image(1, 2, CV_16UC3, cv::Scalar::all(0));
    image.at<cv::Vec3w>(0, 1) = cv::Vec3w(1020, 512, 256); // B, G, R

    // One min and max for all channels (0 and 1020), so the channels keep their relative brightness.
    EXPECT_EQ(displayPixel(image, 0, 0), cv::Vec4b(0, 0, 0, 255));
    EXPECT_EQ(displayPixel(image, 0, 1), cv::Vec4b(64, 128, 255, 255));
}

TEST(DisplayConverter, SignedEightBitIsNormalised) {
    cv::Mat image(1, 2, CV_8SC1);
    image.at<schar>(0, 0) = -128;
    image.at<schar>(0, 1) = 127;

    EXPECT_EQ(displayPixel(image, 0, 0), cv::Vec4b(0, 0, 0, 255));
    EXPECT_EQ(displayPixel(image, 0, 1), cv::Vec4b(255, 255, 255, 255));
}

TEST(DisplayConverter, ConstantImageBecomesBlack) {
    const cv::Mat image(2, 2, CV_32FC1, cv::Scalar(5.0));

    const cv::Mat display = toDisplayImage(image);

    for (int row = 0; row < display.rows; ++row) {
        for (int col = 0; col < display.cols; ++col) {
            EXPECT_EQ(display.at<cv::Vec4b>(row, col), cv::Vec4b(0, 0, 0, 255)) << row << ", " << col;
        }
    }
}

TEST(DisplayConverter, NonFiniteValuesAreIgnoredAndShownBlack) {
    const float values[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), 0.0f, 10.0f};
    const cv::Mat image(1, 5, CV_32FC1, const_cast<float*>(values));

    const cv::Mat display = toDisplayImage(image);

    std::vector<int> red;
    for (int col = 0; col < display.cols; ++col) {
        red.push_back(display.at<cv::Vec4b>(0, col)[0]);
    }
    EXPECT_EQ(red, (std::vector<int>{0, 0, 0, 0, 255}));
}

TEST(DisplayConverter, LeavesOriginalUnmodified) {
    cv::Mat image(3, 3, CV_16UC1);
    cv::randu(image, cv::Scalar(0), cv::Scalar(60000));
    const cv::Mat before = image.clone();

    (void)toDisplayImage(image);

    EXPECT_EQ(cv::norm(image, before, cv::NORM_INF), 0.0);
}

// What the history counts for an image's display copy before converting it.
TEST(DisplayConverter, DisplayBytesAreTheConvertedImages) {
    for (const int type : {CV_8UC1, CV_16UC3, CV_32FC4, CV_64FC2}) {
        const cv::Mat image(5, 7, type, cv::Scalar::all(1));
        const cv::Mat display = toDisplayImage(image);
        EXPECT_EQ(viewer_core::displayBytes(image), display.total() * display.elemSize()) << type;
    }
}

TEST(DisplayConverter, RejectsUnsupportedInput) {
    const int threeDimensions[] = {2, 2, 2};
    const std::vector<std::pair<std::string, cv::Mat>> images = {
        {"empty", cv::Mat()},
        {"five channels", cv::Mat(2, 2, CV_8UC(5), cv::Scalar::all(0))},
        {"16-bit float", cv::Mat(2, 2, CV_16FC1, cv::Scalar(0))},
        {"three dimensions", cv::Mat(3, threeDimensions, CV_8UC1, cv::Scalar(0))},
    };

    for (const auto& [label, image] : images) {
        EXPECT_THROW((void)toDisplayImage(image), std::invalid_argument) << label;
    }
}

// --- At the size it is drawn ------------------------------------------------------------------------------

TEST(DisplayConverter, DrawnSizeHasThatSize) {
    const cv::Mat image(480, 640, CV_8UC3, cv::Scalar(10, 20, 30));

    const cv::Mat drawn = toDisplayImage(image, DisplaySettings{}, cv::Size(64, 48));

    EXPECT_EQ(drawn.size(), cv::Size(64, 48));
    EXPECT_EQ(drawn.type(), CV_8UC4);
    EXPECT_EQ(drawn.at<cv::Vec4b>(24, 32), cv::Vec4b(30, 20, 10, 255));
}

TEST(DisplayConverter, DrawnSizeNeverEnlarges) {
    const cv::Mat image(48, 64, CV_8UC1, cv::Scalar(7));

    EXPECT_EQ(toDisplayImage(image, DisplaySettings{}, cv::Size(1000, 1000)).size(), cv::Size(64, 48));
}

// Auto stretches the whole image's range, not the shrunk picture's: one bright pixel, averaged away when
// shrinking, still sets the top, so the picture looks the same at every size.
TEST(DisplayConverter, DrawnSizeKeepsTheWholeImagesRange) {
    cv::Mat image(1000, 1000, CV_16UC1, cv::Scalar(0));
    image.at<std::uint16_t>(0, 0) = 1000;
    image(cv::Rect(500, 500, 400, 400)).setTo(500);

    const cv::Mat drawn = toDisplayImage(image, DisplaySettings{}, cv::Size(100, 100));

    const int grey = drawn.at<cv::Vec4b>(70, 70)[0];
    EXPECT_GE(grey, 127);
    EXPECT_LE(grey, 128);
}

TEST(DisplayConverter, DrawnSizeTakesEveryDepth) {
    for (const int depth : {CV_8U, CV_8S, CV_16U, CV_16S, CV_32S, CV_32F, CV_64F}) {
        for (const int channels : {1, 3}) {
            const cv::Mat image(480, 640, CV_MAKETYPE(depth, channels), cv::Scalar::all(depth + 1));
            EXPECT_EQ(toDisplayImage(image, DisplaySettings{}, cv::Size(64, 48)).size(), cv::Size(64, 48))
                << depth << " " << channels;
        }
    }
}

// A NaN stays one dark pixel among its neighbours; averaging the original would make its whole block NaN, and
// so black.
TEST(DisplayConverter, DrawnSizeKeepsNonFiniteValuesSmall) {
    cv::Mat image(100, 100, CV_32FC1, cv::Scalar(1.0f));
    image.at<float>(50, 50) = 0.0f;
    image.at<float>(0, 0) = std::numeric_limits<float>::quiet_NaN();

    for (const viewer_core::ColourMap colours :
         {viewer_core::ColourMap::Grey, viewer_core::ColourMap::Turbo}) {
        const cv::Mat drawn = toDisplayImage(image, DisplaySettings{.colours = colours}, cv::Size(10, 10));
        const cv::Mat whole = toDisplayImage(image, DisplaySettings{.colours = colours});
        const cv::Vec4b bright = whole.at<cv::Vec4b>(1, 1);
        const cv::Vec4b corner = drawn.at<cv::Vec4b>(0, 0);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(corner[c], bright[c] * 0.99, 2.0) << static_cast<int>(colours) << " " << c;
        }
    }
}

// (R6) A range whose span does not fit in a double still maps linearly.
TEST(DisplayConverter, WideFixedRangeStillMapsLinearly) {
    cv::Mat image(1, 3, CV_64FC1);
    image.at<double>(0, 0) = -1e308;
    image.at<double>(0, 1) = 0.0;
    image.at<double>(0, 2) = 1e308;
    const DisplaySettings settings{.mode = RangeMode::Fixed, .range = {-1e308, 1e308}};

    const cv::Mat display = toDisplayImage(image, settings);

    EXPECT_EQ(display.at<cv::Vec4b>(0, 0)[0], 0);
    EXPECT_GE(display.at<cv::Vec4b>(0, 1)[0], 127);
    EXPECT_LE(display.at<cv::Vec4b>(0, 1)[0], 128);
    EXPECT_EQ(display.at<cv::Vec4b>(0, 2)[0], 255);
}

// (R6) Doubles beyond a float's range map linearly too, Auto and Fixed alike. Wide enough for OpenCV's vector
// code, which would read them as floats, so infinity.
TEST(DisplayConverter, DoublesBeyondFloatRangeMapLinearly) {
    cv::Mat image(1, 64, CV_64FC1, cv::Scalar(0.0));
    image.at<double>(0, 1) = 5e38;
    image.at<double>(0, 2) = 1e39;
    image.at<double>(0, 3) = -1e39;

    const cv::Mat automatic = toDisplayImage(image);
    EXPECT_NEAR(automatic.at<cv::Vec4b>(0, 1)[0], 191, 1); // -1e39 to 1e39: 5e38 is three quarters
    EXPECT_EQ(automatic.at<cv::Vec4b>(0, 2)[0], 255);

    const cv::Mat fixed =
        toDisplayImage(image, DisplaySettings{.mode = RangeMode::Fixed, .range = {0.0, 1e39}});
    EXPECT_NEAR(fixed.at<cv::Vec4b>(0, 1)[0], 128, 1);
    EXPECT_EQ(fixed.at<cv::Vec4b>(0, 2)[0], 255);
    EXPECT_EQ(fixed.at<cv::Vec4b>(0, 3)[0], 0); // clipped below the range
}

// A long thin image is averaged along its long side too, though its short side is shorter than the shrink:
// every fourth column white shrinks to an even grey, not to stripes.
TEST(DisplayConverter, DrawnSizeAveragesThinImages) {
    cv::Mat image(2, 10000, CV_8UC1, cv::Scalar(0));
    for (int col = 0; col < image.cols; col += 4) {
        image.col(col).setTo(255);
    }

    const cv::Mat drawn = toDisplayImage(image, DisplaySettings{}, cv::Size(800, 1));

    cv::Mat red;
    cv::extractChannel(drawn, red, 0);
    double darkest = 0.0;
    double brightest = 0.0;
    cv::minMaxLoc(red, &darkest, &brightest);
    EXPECT_NEAR(darkest, 255.0 / 4, 2.0);
    EXPECT_NEAR(brightest, 255.0 / 4, 2.0);
}

// --- The HDR range -----------------------------------------------------------------------------------------

const viewer_core::ImageTags kHlg{.valueRange = {0, 65535},
                                  .transfer = viewer_core::Transfer::Hlg,
                                  .primaries = viewer_core::Primaries::Bt2020};
const DisplaySettings kHdr{.mode = RangeMode::Hdr};

cv::Mat hdrSignal(int rows, int cols) {
    cv::Mat signal(rows, cols, CV_16UC3);
    cv::randu(signal, cv::Scalar::all(0), cv::Scalar::all(65536));
    return signal;
}

// The tone map's BGR as the display's RGBA.
cv::Mat rgbaOf(const cv::Mat& bgr) {
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    return rgba;
}

TEST(DisplayConverter, HdrModeUsesTheToneMap) {
    const cv::Mat signal = hdrSignal(4, 5);

    const cv::Mat display = toDisplayImage(signal, kHdr, {.tags = kHlg});

    const cv::Mat expected = rgbaOf(viewer_core::toneMapToSdr(signal, viewer_core::Transfer::Hlg,
                                                              viewer_core::Primaries::Bt2020, {0, 65535}));
    EXPECT_EQ(cv::norm(display, expected, cv::NORM_INF), 0.0);
}

TEST(DisplayConverter, HdrWithoutATagShowsAsAuto) {
    const cv::Mat signal = hdrSignal(4, 5);

    EXPECT_EQ(cv::norm(toDisplayImage(signal, kHdr, {.tags = {.valueRange = {0, 65535}}}),
                       toDisplayImage(signal, DisplaySettings{}, {.tags = {.valueRange = {0, 65535}}}),
                       cv::NORM_INF),
              0.0);
}

// Tone-mapped at the drawn size: each pixel is the map of its block's average.
TEST(DisplayConverter, HdrShrinksBeforeMapping) {
    const cv::Mat signal = hdrSignal(3000, 4000);

    const cv::Mat display = toDisplayImage(signal, kHdr, {.size = {400, 300}, .tags = kHlg});

    ASSERT_EQ(display.size(), cv::Size(400, 300));
    const cv::Scalar mean = cv::mean(signal(cv::Rect(2000, 1500, 10, 10)));
    const cv::Mat block(1, 1, CV_16UC3,
                        cv::Scalar(std::round(mean[0]), std::round(mean[1]), std::round(mean[2])));
    const cv::Vec4b expected = rgbaOf(viewer_core::toneMapToSdr(block, viewer_core::Transfer::Hlg,
                                                                viewer_core::Primaries::Bt2020, {0, 65535}))
                                   .at<cv::Vec4b>(0, 0);
    const cv::Vec4b centre = display.at<cv::Vec4b>(150, 200);
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_NEAR(centre[channel], expected[channel], 2) << channel;
    }
}

TEST(DisplayConverter, HdrKeepsAlphaAndShowsTwoChannelsAsAuto) {
    cv::Mat withAlpha(2, 2, CV_16UC4, cv::Scalar(30000, 30000, 30000, 65535));
    const cv::Mat twoChannels(2, 2, CV_16UC2, cv::Scalar(100, 900));

    EXPECT_EQ(toDisplayImage(withAlpha, kHdr, {.tags = kHlg}).at<cv::Vec4b>(0, 0)[3], 255);
    EXPECT_EQ(cv::norm(toDisplayImage(twoChannels, kHdr, {.tags = kHlg}),
                       toDisplayImage(twoChannels, DisplaySettings{}, {.tags = kHlg}), cv::NORM_INF),
              0.0);
}

// --- Clipping ----------------------------------------------------------------------------------------------

cv::Mat rowOf(int type, std::initializer_list<double> values) {
    cv::Mat row(1, static_cast<int>(values.size()), type);
    int x = 0;
    for (const double value : values) {
        row.col(x++).setTo(value);
    }
    return row;
}

cv::Vec3b rgbAt(const cv::Mat& display, int x, int y = 0) {
    const cv::Vec4b pixel = display.at<cv::Vec4b>(y, x);
    return {pixel[0], pixel[1], pixel[2]};
}

const viewer_core::DisplayOptions kShowClipping{.showClipping = true};

TEST(DisplayConverter, ClippingMarksBothEnds) {
    const cv::Mat display = toDisplayImage(rowOf(CV_8UC1, {0, 128, 255}), DisplaySettings{}, kShowClipping);

    EXPECT_EQ(rgbAt(display, 0), viewer_core::kClippedBelowGrey);
    EXPECT_EQ(rgbAt(display, 1), cv::Vec3b(128, 128, 128));
    EXPECT_EQ(rgbAt(display, 2), viewer_core::kClippedAbove);
}

TEST(DisplayConverter, BelowIsWhiteUnderAColourMap) {
    const cv::Mat display = toDisplayImage(
        rowOf(CV_8UC1, {0, 128}), DisplaySettings{.colours = viewer_core::ColourMap::Turbo}, kShowClipping);

    EXPECT_EQ(rgbAt(display, 0), viewer_core::kClippedBelowMapped);
}

// A clipped pixel stays visible however small the picture: its block is marked.
TEST(DisplayConverter, ClippingSurvivesShrinking) {
    cv::Mat image(100, 100, CV_8UC1, cv::Scalar(128));
    image.at<uchar>(37, 55) = 255;

    const cv::Mat display =
        toDisplayImage(image, DisplaySettings{}, {.size = {10, 10}, .showClipping = true});

    ASSERT_EQ(display.size(), cv::Size(10, 10));
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            EXPECT_EQ(rgbAt(display, x, y) == viewer_core::kClippedAbove, x == 5 && y == 3) << x << "," << y;
        }
    }
}

// Auto without a hint stretches the image's own extremes to black and white, losing nothing.
TEST(DisplayConverter, StretchedAutoMarksNothing) {
    const cv::Mat image = rowOf(CV_16UC1, {100, 200, 300});

    EXPECT_EQ(cv::norm(toDisplayImage(image, DisplaySettings{}, kShowClipping), toDisplayImage(image),
                       cv::NORM_INF),
              0.0);
}

TEST(DisplayConverter, FixedMarksBeyondItsEnds) {
    const DisplaySettings fixed{.mode = RangeMode::Fixed, .range = {0, 1}};

    const cv::Mat display = toDisplayImage(
        rowOf(CV_32FC1, {-1, 0.5, 2, std::numeric_limits<double>::quiet_NaN()}), fixed, kShowClipping);

    EXPECT_EQ(rgbAt(display, 0), viewer_core::kClippedBelowGrey);
    EXPECT_NE(rgbAt(display, 1), viewer_core::kClippedBelowGrey);
    EXPECT_NE(rgbAt(display, 1), viewer_core::kClippedAbove);
    EXPECT_EQ(rgbAt(display, 2), viewer_core::kClippedAbove);
    EXPECT_EQ(rgbAt(display, 3), cv::Vec3b(0, 0, 0)); // NaN is never marked
}

// HDR's top is the camera's own clip; a super-white is not clipped there, though Auto with the hint clips it.
TEST(DisplayConverter, HdrMarksTheScaleTop) {
    const viewer_core::ImageTags tags{.valueRange = {4100, 60218},
                                      .transfer = viewer_core::Transfer::Hlg,
                                      .primaries = viewer_core::Primaries::Bt2020};
    const cv::Mat image = rowOf(CV_16UC3, {65535, 64000, 4100, 30000});

    const cv::Mat hdr = toDisplayImage(image, kHdr, {.tags = tags, .showClipping = true});
    const cv::Mat automatic = toDisplayImage(image, DisplaySettings{}, {.tags = tags, .showClipping = true});

    EXPECT_EQ(rgbAt(hdr, 0), viewer_core::kClippedAbove);
    EXPECT_NE(rgbAt(hdr, 1), viewer_core::kClippedAbove);
    EXPECT_EQ(rgbAt(hdr, 2), viewer_core::kClippedBelowGrey);
    EXPECT_NE(rgbAt(hdr, 3), viewer_core::kClippedAbove);
    EXPECT_EQ(rgbAt(automatic, 1), viewer_core::kClippedAbove);
}

double distance(const cv::Vec3b& a, const cv::Vec3b& b) {
    return cv::norm(cv::Vec3d(a[0], a[1], a[2]) - cv::Vec3d(b[0], b[1], b[2]));
}

// Every entry of a map, as RGB.
std::vector<cv::Vec3b> mapColours(viewer_core::ColourMap colours) {
    cv::Mat ramp(1, 256, CV_8UC1);
    for (int x = 0; x < 256; ++x) {
        ramp.at<uchar>(0, x) = static_cast<uchar>(x);
    }
    const cv::Mat display = toDisplayImage(ramp, DisplaySettings{.colours = colours});
    std::vector<cv::Vec3b> entries;
    for (int x = 0; x < 256; ++x) {
        entries.push_back(rgbAt(display, x));
    }
    return entries;
}

TEST(DisplayConverter, ClippingColoursStandOutFromTheMaps) {
    using viewer_core::ColourMap;
    const std::vector<std::pair<ColourMap, cv::Vec3b>> uses{
        {ColourMap::Grey, viewer_core::kClippedAbove},
        {ColourMap::Grey, viewer_core::kClippedBelowGrey},
        {ColourMap::Turbo, viewer_core::kClippedAbove},
        {ColourMap::Turbo, viewer_core::kClippedBelowMapped},
        {ColourMap::Viridis, viewer_core::kClippedAbove},
        {ColourMap::Viridis, viewer_core::kClippedBelowMapped}};

    for (const auto& [colours, mark] : uses) {
        for (const cv::Vec3b& entry : mapColours(colours)) {
            EXPECT_GE(distance(mark, entry), 100.0) << static_cast<int>(colours) << " " << mark;
        }
    }
    EXPECT_GE(distance(viewer_core::kClippedAbove, viewer_core::kClippedBelowGrey), 100.0);
    EXPECT_GE(distance(viewer_core::kClippedAbove, viewer_core::kClippedBelowMapped), 100.0);
}

TEST(SizeWithin, KeepsTheAspect) {
    EXPECT_EQ(sizeWithin({4000, 3000}, {1024, 1024}), cv::Size(1024, 768));
    EXPECT_EQ(sizeWithin({3000, 4000}, {1024, 1024}), cv::Size(768, 1024));
    EXPECT_EQ(sizeWithin({64, 48}, {1024, 1024}), cv::Size(64, 48)); // never larger
    EXPECT_EQ(sizeWithin({10000, 1}, {100, 100}), cv::Size(100, 1)); // never empty
}

} // namespace
