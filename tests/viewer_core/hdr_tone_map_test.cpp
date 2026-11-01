#include "viewer_core/display_converter.h"
#include "viewer_core/hdr_tone_map.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace {

using viewer_core::DisplayRange;
using viewer_core::Primaries;
using viewer_core::toneMapToSdr;
using viewer_core::Transfer;

constexpr DisplayRange kFullScale{0, 65535};
// A 10-bit limited-range video in 16 bits with its top code at 65535: black 64 and nominal white 940 of 1023.
constexpr DisplayRange kVideoRange{4100, 60218};

// One row of 3-channel 16-bit greys.
cv::Mat greys(const std::vector<int>& values) {
    cv::Mat row(1, static_cast<int>(values.size()), CV_16UC3);
    for (int x = 0; x < row.cols; ++x) {
        row.at<cv::Vec3w>(0, x) = cv::Vec3w::all(static_cast<ushort>(values[static_cast<std::size_t>(x)]));
    }
    return row;
}

// The value `share` of the way from the range's black to its nominal white.
int level(const DisplayRange& range, double share) {
    return static_cast<int>(std::lround(range.min + share * (range.max - range.min)));
}

// The 16-bit signal of a PQ light level: the inverse of the EOTF (ITU-R BT.2100), on the full 16-bit scale.
int pqSignal(double candelas) {
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 32.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 128.0;
    const double c3 = 2392.0 / 128.0;
    const double y = std::pow(candelas / 10000.0, m1);
    return static_cast<int>(std::lround(65535.0 * std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2)));
}

// The red of each output pixel (BGR order): for a grey, its level.
std::vector<int> reds(const cv::Mat& mapped) {
    std::vector<int> values;
    for (int x = 0; x < mapped.cols; ++x) {
        values.push_back(mapped.at<cv::Vec3b>(0, x)[2]);
    }
    return values;
}

std::vector<int> hlg(const std::vector<int>& values, const DisplayRange& range = kFullScale) {
    return reds(toneMapToSdr(greys(values), Transfer::Hlg, Primaries::Bt2020, range));
}

std::vector<int> pq(const std::vector<int>& values) {
    return reds(toneMapToSdr(greys(values), Transfer::Pq, Primaries::Bt2020, kFullScale));
}

TEST(HdrToneMap, BlackStaysBlack) {
    EXPECT_EQ(hlg({0}), std::vector<int>{0});
    EXPECT_EQ(pq({0}), std::vector<int>{0});
    const cv::Mat mapped = toneMapToSdr(greys({0}), Transfer::Hlg, Primaries::Bt2020, kFullScale);
    EXPECT_EQ(mapped.type(), CV_8UC3);
    EXPECT_EQ(mapped.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 0, 0));
}

TEST(HdrToneMap, PeakIsWhite) {
    EXPECT_EQ(hlg({65535}), std::vector<int>{255});
    EXPECT_EQ(hlg({65535}, kVideoRange), std::vector<int>{255});
    EXPECT_EQ(pq({pqSignal(1000), pqSignal(4000)}), (std::vector<int>{255, 255}));
}

// HLG 75 % is HDR reference white (203 cd/m², ITU-R BT.2408): just below the screen's white, leaving room
// above.
TEST(HdrToneMap, ReferenceWhiteSitsJustBelowWhite) {
    const int referenceWhite = hlg({level(kFullScale, 0.75)}).front();

    EXPECT_GE(referenceWhite, 190);
    EXPECT_LE(referenceWhite, 235);
}

// A phone's glare lies above nominal white: it must not all turn white.
TEST(HdrToneMap, SuperWhitesKeepTheirGradations) {
    const std::vector<int> mapped = hlg({60218, 62000, 65535}, kVideoRange);

    EXPECT_GE(mapped[0], 225);
    EXPECT_LE(mapped[0], 250);
    EXPECT_LT(mapped[0], mapped[1]);
    EXPECT_LT(mapped[1], mapped[2]);
}

TEST(HdrToneMap, PqReferenceWhiteMatchesHlg) {
    EXPECT_NEAR(pq({pqSignal(203)}).front(), hlg({level(kFullScale, 0.75)}).front(), 10);
}

TEST(HdrToneMap, Monotonic) {
    std::vector<int> values;
    for (int i = 0; i <= 256; ++i) {
        values.push_back(std::min(i * 256, 65535));
    }

    for (const std::vector<int>& mapped : {hlg(values), hlg(values, kVideoRange), pq(values)}) {
        for (std::size_t i = 1; i < mapped.size(); ++i) {
            EXPECT_LE(mapped[i - 1], mapped[i]) << i;
        }
    }
}

TEST(HdrToneMap, Bt2020RedStaysRed) {
    cv::Mat red(1, 1, CV_16UC3, cv::Scalar(0, 0, level(kFullScale, 0.5)));

    const cv::Vec3b mapped =
        toneMapToSdr(red, Transfer::Hlg, Primaries::Bt2020, kFullScale).at<cv::Vec3b>(0, 0);

    EXPECT_GT(mapped[2], 100);
    EXPECT_LE(mapped[1], 10);
    EXPECT_LE(mapped[0], 10);
}

// The hint, not the depth, sets the levels: 10-bit codes in 16 bits look as the same levels on the full scale
// do. Their white point is above nominal white (the most headroom a limited-range video carries), since the
// codes above the hint could be super-whites; so nominal white is just below the screen's white, not at it.
TEST(HdrToneMap, TenBitHintOnSixteenBits) {
    constexpr DisplayRange kTenBits{0, 1023};

    const std::vector<int> mapped = hlg({0, level(kTenBits, 0.75), 1023}, kTenBits);

    EXPECT_EQ(mapped[0], 0);
    EXPECT_NEAR(mapped[1], hlg({level(kFullScale, 0.75)}).front(), 10);
    EXPECT_GE(mapped[2], 225);
    EXPECT_LE(mapped[2], 250);
}

TEST(HdrToneMap, SingleChannelMatchesGrey) {
    const std::vector<int> values{0, 20000, 49151, 65535};
    cv::Mat single;
    cv::extractChannel(greys(values), single, 0);

    const cv::Mat mapped = toneMapToSdr(single, Transfer::Hlg, Primaries::Bt2020, kFullScale);

    ASSERT_EQ(mapped.type(), CV_8UC1);
    const std::vector<int> expected = hlg(values);
    for (int x = 0; x < mapped.cols; ++x) {
        EXPECT_NEAR(mapped.at<uchar>(0, x), expected[static_cast<std::size_t>(x)], 1) << x;
    }
}

TEST(HdrToneMap, FloatSignalMatchesSixteenBits) {
    const std::vector<int> values{0, 20000, 49151, 65535};
    cv::Mat signal;
    greys(values).convertTo(signal, CV_32F, 1.0 / 65535.0);

    const std::vector<int> mapped = reds(toneMapToSdr(signal, Transfer::Hlg, Primaries::Bt2020, {0, 1}));

    const std::vector<int> expected = hlg(values);
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_NEAR(mapped[i], expected[i], 1) << i;
    }
}

// (F1) A range as wide as a double allows, which the wire format accepts, still maps linearly: its middle
// shows as the middle of 0 to 1 does. Its span overflowed to infinity, and the white point became NaN.
TEST(HdrToneMap, WidestRangeMapsLinearly) {
    constexpr double kMax = std::numeric_limits<double>::max();
    const cv::Mat middle(1, 1, CV_64FC3, cv::Scalar::all(0.0));
    const cv::Mat half(1, 1, CV_64FC3, cv::Scalar::all(0.5));

    const cv::Vec3b widest =
        toneMapToSdr(middle, Transfer::Hlg, Primaries::Bt2020, {-kMax, kMax}).at<cv::Vec3b>(0, 0);
    const cv::Vec3b unit = toneMapToSdr(half, Transfer::Hlg, Primaries::Bt2020, {0, 1}).at<cv::Vec3b>(0, 0);

    EXPECT_EQ(widest, unit);
}

// (F1) NaN is black and infinities clip, as everywhere else. (A NaN once reached the sRGB table as an index:
// 0 on ARM, which hid it, but far out of bounds on x86-64.)
TEST(HdrToneMap, NonFiniteValuesClip) {
    cv::Mat signal(1, 3, CV_32FC3);
    signal.at<cv::Vec3f>(0, 0) = cv::Vec3f::all(std::numeric_limits<float>::quiet_NaN());
    signal.at<cv::Vec3f>(0, 1) = cv::Vec3f::all(std::numeric_limits<float>::infinity());
    signal.at<cv::Vec3f>(0, 2) = cv::Vec3f::all(-std::numeric_limits<float>::infinity());

    const std::vector<int> mapped = reds(toneMapToSdr(signal, Transfer::Hlg, Primaries::Bt2020, {0, 1}));

    EXPECT_EQ(mapped, (std::vector<int>{0, 255, 0}));
}

} // namespace
