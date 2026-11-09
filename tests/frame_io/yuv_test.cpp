#include "frame_io/yuv.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using frame_io::YuvLayout;
using frame_io::YuvMatrix;
using frame_io::YuvPlanes;
using frame_io::YuvRange;

// An NV12 picture of one Y, U and V everywhere.
struct Nv12 {
    Nv12(int width, int height, std::uint8_t y, std::uint8_t u, std::uint8_t v)
        : luma(static_cast<std::size_t>(width * height), y),
          chroma(static_cast<std::size_t>(width * height / 2)) {
        for (std::size_t i = 0; i < chroma.size(); i += 2) {
            chroma[i] = u;
            chroma[i + 1] = v;
        }
        planes = {.layout = YuvLayout::Nv12,
                  .size = {width, height},
                  .data = {luma.data(), chroma.data(), nullptr},
                  .stride = {width, width, 0}};
    }
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;
    YuvPlanes planes;
};

cv::Vec3b bgrOf(std::uint8_t y, std::uint8_t u, std::uint8_t v, YuvMatrix matrix, YuvRange range) {
    const Nv12 picture(4, 2, y, u, v);
    const cv::Mat bgr = frame_io::yuvToBgr(picture.planes, matrix, range);
    EXPECT_EQ(bgr.type(), CV_8UC3);
    EXPECT_EQ(bgr.size(), cv::Size(4, 2));
    return bgr.at<cv::Vec3b>(1, 3);
}

void expectBgrNear(const cv::Vec3b& actual, double b, double g, double r, double tolerance) {
    EXPECT_NEAR(actual[0], b, tolerance);
    EXPECT_NEAR(actual[1], g, tolerance);
    EXPECT_NEAR(actual[2], r, tolerance);
}

// Video range: Y 16 is black and 235 white; (126 − 16) / 219 × 255 is 128.08.
TEST(Yuv, VideoRangeWhiteAndBlack) {
    expectBgrNear(bgrOf(235, 128, 128, YuvMatrix::Bt601, YuvRange::Video), 255, 255, 255, 1);
    expectBgrNear(bgrOf(16, 128, 128, YuvMatrix::Bt601, YuvRange::Video), 0, 0, 0, 1);
    expectBgrNear(bgrOf(126, 128, 128, YuvMatrix::Bt601, YuvRange::Video), 128, 128, 128, 1);
}

// Full range keeps the codes above 235, where a camera's highlights are; taken for video range they would all
// be white, and glare could not be told from paper.
TEST(Yuv, FullRangeKeepsHighlights) {
    expectBgrNear(bgrOf(255, 128, 128, YuvMatrix::Bt601, YuvRange::Full), 255, 255, 255, 1);
    expectBgrNear(bgrOf(235, 128, 128, YuvMatrix::Bt601, YuvRange::Full), 235, 235, 235, 1);
}

// (Y, Cb, Cr) = (100, 90, 200) in video range, worked out from each matrix's formula: nothing clips, and the
// two matrices give greens 13 apart.
TEST(Yuv, Bt709AgainstFormula) {
    expectBgrNear(bgrOf(100, 90, 200, YuvMatrix::Bt709, YuvRange::Video), 17.54, 67.54, 226.89, 1);
    expectBgrNear(bgrOf(100, 90, 200, YuvMatrix::Bt601, YuvRange::Video), 21.15, 54.16, 212.72, 1);
}

// One 4×2 picture in every layout: the same BGR whichever way its planes are laid out.
TEST(Yuv, EveryLayoutGivesTheSamePicture) {
    const std::array<std::uint8_t, 8> y{16, 60, 120, 200, 235, 180, 90, 30};
    const std::array<std::uint8_t, 2> u{100, 160}; // per pair of columns, the same on both rows
    const std::array<std::uint8_t, 2> v{150, 90};

    std::vector<std::uint8_t> nv12uv{u[0], v[0], u[1], v[1]};
    std::vector<std::uint8_t> nv21vu{v[0], u[0], v[1], u[1]};
    std::vector<std::uint8_t> yuyv;
    std::vector<std::uint8_t> uyvy;
    for (int row = 0; row < 2; ++row) {
        for (int pair = 0; pair < 2; ++pair) {
            const auto y0 = y[static_cast<std::size_t>(row * 4 + pair * 2)];
            const auto y1 = y[static_cast<std::size_t>(row * 4 + pair * 2 + 1)];
            yuyv.insert(yuyv.end(),
                        {y0, u[static_cast<std::size_t>(pair)], y1, v[static_cast<std::size_t>(pair)]});
            uyvy.insert(uyvy.end(),
                        {u[static_cast<std::size_t>(pair)], y0, v[static_cast<std::size_t>(pair)], y1});
        }
    }
    std::vector<std::uint8_t> uPlane{u[0], u[1]};
    std::vector<std::uint8_t> vPlane{v[0], v[1]};

    const YuvPlanes nv12{.layout = YuvLayout::Nv12,
                         .size = {4, 2},
                         .data = {y.data(), nv12uv.data(), nullptr},
                         .stride = {4, 4, 0}};
    const std::vector<YuvPlanes> layouts{{.layout = YuvLayout::Nv21,
                                          .size = {4, 2},
                                          .data = {y.data(), nv21vu.data(), nullptr},
                                          .stride = {4, 4, 0}},
                                         {.layout = YuvLayout::Yuyv,
                                          .size = {4, 2},
                                          .data = {yuyv.data(), nullptr, nullptr},
                                          .stride = {8, 0, 0}},
                                         {.layout = YuvLayout::Uyvy,
                                          .size = {4, 2},
                                          .data = {uyvy.data(), nullptr, nullptr},
                                          .stride = {8, 0, 0}},
                                         {.layout = YuvLayout::I420,
                                          .size = {4, 2},
                                          .data = {y.data(), uPlane.data(), vPlane.data()},
                                          .stride = {4, 2, 2}},
                                         {.layout = YuvLayout::Yv12,
                                          .size = {4, 2},
                                          .data = {y.data(), vPlane.data(), uPlane.data()},
                                          .stride = {4, 2, 2}}};
    // BT.601 video goes through OpenCV's conversions, BT.709 full range through the lab's own.
    for (const auto& [matrix, range] :
         {std::pair{YuvMatrix::Bt601, YuvRange::Video}, std::pair{YuvMatrix::Bt709, YuvRange::Full}}) {
        const cv::Mat reference = frame_io::yuvToBgr(nv12, matrix, range);
        ASSERT_EQ(reference.size(), cv::Size(4, 2));
        for (const YuvPlanes& planes : layouts) {
            const cv::Mat bgr = frame_io::yuvToBgr(planes, matrix, range);
            EXPECT_LE(cv::norm(bgr, reference, cv::NORM_INF), 1.0)
                << static_cast<int>(planes.layout) << " " << static_cast<int>(matrix);
        }
    }
}

// A 4:2:0 or 4:2:2 picture needs an even width (and 4:2:0 an even height); a frame that breaks that, or has
// no data, gives no picture rather than reading past its planes.
TEST(Yuv, MalformedPlanesGiveNoPicture) {
    std::vector<std::uint8_t> bytes(64, 128);
    const auto planes = [&](YuvLayout layout, cv::Size size) {
        return YuvPlanes{.layout = layout,
                         .size = size,
                         .data = {bytes.data(), bytes.data(), bytes.data()},
                         .stride = {8, 8, 8}};
    };

    EXPECT_TRUE(
        frame_io::yuvToBgr(planes(YuvLayout::Nv12, {3, 2}), YuvMatrix::Bt601, YuvRange::Video).empty());
    EXPECT_TRUE(
        frame_io::yuvToBgr(planes(YuvLayout::I420, {4, 3}), YuvMatrix::Bt709, YuvRange::Full).empty());
    EXPECT_TRUE(
        frame_io::yuvToBgr(planes(YuvLayout::Yuyv, {5, 2}), YuvMatrix::Bt601, YuvRange::Video).empty());
    EXPECT_TRUE(
        frame_io::yuvToBgr(planes(YuvLayout::Nv12, {0, 0}), YuvMatrix::Bt601, YuvRange::Video).empty());
    YuvPlanes missing = planes(YuvLayout::Nv12, {4, 2});
    missing.data[1] = nullptr;
    EXPECT_TRUE(frame_io::yuvToBgr(missing, YuvMatrix::Bt601, YuvRange::Video).empty());
    EXPECT_TRUE(frame_io::p010ToBgr16(nullptr, 8, bytes.data(), 8, {4, 2}, YuvMatrix::Bt709, YuvRange::Video)
                    .empty());
}

// P010 on FfmpegVideo's scale, codes × 65535 / 1023: video range's black (64) and white (940) land at 4100
// and 60218, and a code above white (1019) keeps its value instead of clipping.
TEST(Yuv, P010OnFfmpegVideosScale) {
    const auto grey = [](int code) {
        std::vector<std::uint16_t> luma(8, static_cast<std::uint16_t>(code << 6));
        std::vector<std::uint16_t> chroma(4, static_cast<std::uint16_t>(512 << 6));
        const cv::Mat bgr = frame_io::p010ToBgr16(reinterpret_cast<const std::uint8_t*>(luma.data()), 8,
                                                  reinterpret_cast<const std::uint8_t*>(chroma.data()), 8,
                                                  cv::Size(4, 2), YuvMatrix::Bt709, YuvRange::Video);
        EXPECT_EQ(bgr.type(), CV_16UC3);
        return bgr.at<cv::Vec3w>(1, 2);
    };

    for (const int channel : {0, 1, 2}) {
        EXPECT_NEAR(grey(940)[channel], 60218, 64);
        EXPECT_NEAR(grey(64)[channel], 4100, 64);
        EXPECT_NEAR(grey(1019)[channel], 65279, 64);
    }
    EXPECT_EQ(frame_io::p010ValueRange(YuvRange::Video), (visual_debug::ValueRange{4100, 60218}));
    EXPECT_EQ(frame_io::p010ValueRange(YuvRange::Full), (visual_debug::ValueRange{0, 65535}));
}

} // namespace
