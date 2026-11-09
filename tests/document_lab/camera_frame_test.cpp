#include "camera_frame.h"

#include <frame_io/yuv.h>

#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QtGlobal>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using PixelFormat = QVideoFrameFormat::PixelFormat;

// A frame of the format, its planes filled by `fill` while mapped for writing.
QVideoFrame frameOf(const QVideoFrameFormat& format, const std::function<void(QVideoFrame&)>& fill) {
    QVideoFrame frame(format);
    EXPECT_TRUE(frame.map(QVideoFrame::WriteOnly));
    fill(frame);
    frame.unmap();
    return frame;
}

// Copies `rows` rows of `rowBytes` bytes into a plane, row by row at the plane's own stride.
void fillPlane(QVideoFrame& frame, int plane, const std::uint8_t* bytes, int rowBytes, int rows) {
    for (int row = 0; row < rows; ++row) {
        std::memcpy(frame.bits(plane) + static_cast<std::ptrdiff_t>(row) * frame.bytesPerLine(plane),
                    bytes + static_cast<std::ptrdiff_t>(row) * rowBytes, static_cast<std::size_t>(rowBytes));
    }
}

void setRotation90(QVideoFrame& frame) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    frame.setRotation(QtVideo::Rotation::Clockwise90);
#else
    frame.setRotationAngle(QVideoFrame::Rotation90);
#endif
}

cv::Mat convert(const QVideoFrame& frame, document_lab::CameraGeometry geometry = {}) {
    std::string error;
    const std::optional<frame_io::Frame> converted = document_lab::toFrame(frame, geometry, 5ms, error);
    EXPECT_TRUE(converted) << error;
    if (!converted) {
        return {};
    }
    EXPECT_EQ(converted->at, 5ms);
    return converted->image;
}

// One 4×2 picture, as yuv_test.cpp's: a luma per pixel, a U and a V per pair of columns.
const std::array<std::uint8_t, 8> kY{16, 60, 120, 200, 235, 180, 90, 30};
const std::array<std::uint8_t, 2> kU{100, 160};
const std::array<std::uint8_t, 2> kV{150, 90};

QVideoFrame yuvFrame(PixelFormat pixelFormat) {
    const QVideoFrameFormat format(QSize(4, 2), pixelFormat);
    return frameOf(format, [pixelFormat](QVideoFrame& frame) {
        const std::array<std::uint8_t, 4> uv{kU[0], kV[0], kU[1], kV[1]};
        const std::array<std::uint8_t, 4> vu{kV[0], kU[0], kV[1], kU[1]};
        switch (pixelFormat) {
        case PixelFormat::Format_NV12:
            fillPlane(frame, 0, kY.data(), 4, 2);
            fillPlane(frame, 1, uv.data(), 4, 1);
            break;
        case PixelFormat::Format_NV21:
            fillPlane(frame, 0, kY.data(), 4, 2);
            fillPlane(frame, 1, vu.data(), 4, 1);
            break;
        case PixelFormat::Format_YUYV:
        case PixelFormat::Format_UYVY: {
            std::vector<std::uint8_t> packed;
            for (int row = 0; row < 2; ++row) {
                for (std::size_t pair = 0; pair < 2; ++pair) {
                    const auto y0 = kY[static_cast<std::size_t>(row) * 4 + pair * 2];
                    const auto y1 = kY[static_cast<std::size_t>(row) * 4 + pair * 2 + 1];
                    if (pixelFormat == PixelFormat::Format_YUYV) {
                        packed.insert(packed.end(), {y0, kU[pair], y1, kV[pair]});
                    } else {
                        packed.insert(packed.end(), {kU[pair], y0, kV[pair], y1});
                    }
                }
            }
            fillPlane(frame, 0, packed.data(), 8, 2);
            break;
        }
        case PixelFormat::Format_YUV420P:
            fillPlane(frame, 0, kY.data(), 4, 2);
            fillPlane(frame, 1, kU.data(), 2, 1);
            fillPlane(frame, 2, kV.data(), 2, 1);
            break;
        case PixelFormat::Format_YV12:
            fillPlane(frame, 0, kY.data(), 4, 2);
            fillPlane(frame, 1, kV.data(), 2, 1);
            fillPlane(frame, 2, kU.data(), 2, 1);
            break;
        default:
            ADD_FAILURE() << "not a YUV format of this test";
        }
    });
}

// A 4×2 BGR picture, each pixel its own colour.
cv::Mat bgrPicture() {
    cv::Mat picture(2, 4, CV_8UC3);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            picture.at<cv::Vec3b>(y, x) =
                cv::Vec3b(static_cast<std::uint8_t>(10 + x * 40), static_cast<std::uint8_t>(20 + y * 100),
                          static_cast<std::uint8_t>(200 - x * 30));
        }
    }
    return picture;
}

// The picture as a four-byte format, its bytes in memory in `order` ('B', 'G', 'R', or 'A' for the 255 byte).
QVideoFrame fourByteFrame(PixelFormat pixelFormat, const std::array<char, 4>& order, const cv::Mat& picture,
                          QVideoFrameFormat::Direction direction = QVideoFrameFormat::TopToBottom) {
    QVideoFrameFormat format(QSize(picture.cols, picture.rows), pixelFormat);
    format.setScanLineDirection(direction);
    return frameOf(format, [&](QVideoFrame& frame) {
        for (int y = 0; y < picture.rows; ++y) {
            std::uint8_t* row = frame.bits(0) + static_cast<std::ptrdiff_t>(y) * frame.bytesPerLine(0);
            for (int x = 0; x < picture.cols; ++x) {
                const cv::Vec3b bgr = picture.at<cv::Vec3b>(y, x);
                for (std::size_t i = 0; i < 4; ++i) {
                    const char channel = order[i];
                    row[x * 4 + static_cast<int>(i)] = channel == 'B'   ? bgr[0]
                                                       : channel == 'G' ? bgr[1]
                                                       : channel == 'R' ? bgr[2]
                                                                        : std::uint8_t{255};
                }
            }
        }
    });
}

// The C920 sends UYVY when NV12 is chosen: each frame is converted by its own format.
TEST(CameraFrame, ConvertsByTheFramesOwnFormat) {
    const cv::Mat reference = convert(yuvFrame(PixelFormat::Format_NV12));
    ASSERT_EQ(reference.size(), cv::Size(4, 2));
    for (const PixelFormat format :
         {PixelFormat::Format_NV21, PixelFormat::Format_YUYV, PixelFormat::Format_UYVY,
          PixelFormat::Format_YUV420P, PixelFormat::Format_YV12}) {
        EXPECT_LE(cv::norm(convert(yuvFrame(format)), reference, cv::NORM_INF), 2.0)
            << QVideoFrameFormat::pixelFormatToString(format).toStdString();
    }

    const cv::Mat picture = bgrPicture();
    const std::vector<std::pair<PixelFormat, std::array<char, 4>>> fourByte{
        {PixelFormat::Format_BGRA8888, {'B', 'G', 'R', 'A'}},
        {PixelFormat::Format_BGRX8888, {'B', 'G', 'R', 'A'}},
        {PixelFormat::Format_BGRA8888_Premultiplied, {'B', 'G', 'R', 'A'}},
        {PixelFormat::Format_RGBA8888, {'R', 'G', 'B', 'A'}},
        {PixelFormat::Format_RGBX8888, {'R', 'G', 'B', 'A'}},
        {PixelFormat::Format_ARGB8888, {'A', 'R', 'G', 'B'}},
        {PixelFormat::Format_ARGB8888_Premultiplied, {'A', 'R', 'G', 'B'}},
        {PixelFormat::Format_XRGB8888, {'A', 'R', 'G', 'B'}},
        {PixelFormat::Format_ABGR8888, {'A', 'B', 'G', 'R'}},
        {PixelFormat::Format_XBGR8888, {'A', 'B', 'G', 'R'}}};
    for (const auto& [format, order] : fourByte) {
        const cv::Mat converted = convert(fourByteFrame(format, order, picture));
        ASSERT_EQ(converted.type(), CV_8UC3);
        EXPECT_EQ(cv::norm(converted, picture, cv::NORM_INF), 0.0)
            << QVideoFrameFormat::pixelFormatToString(format).toStdString();
    }
}

TEST(CameraFrame, HonoursRangeAndMatrix) {
    QVideoFrameFormat full(QSize(4, 2), PixelFormat::Format_NV12);
    full.setColorRange(QVideoFrameFormat::ColorRange_Full);
    const QVideoFrame highlights = frameOf(full, [](QVideoFrame& frame) {
        const std::array<std::uint8_t, 8> y{235, 235, 235, 235, 235, 235, 235, 235};
        const std::array<std::uint8_t, 4> uv{128, 128, 128, 128};
        fillPlane(frame, 0, y.data(), 4, 2);
        fillPlane(frame, 1, uv.data(), 4, 1);
    });
    const cv::Vec3b grey = convert(highlights).at<cv::Vec3b>(1, 1);
    EXPECT_NEAR(grey[0], 235, 1);
    EXPECT_NEAR(grey[2], 235, 1);

    const std::array<std::uint8_t, 4> uv{kU[0], kV[0], kU[1], kV[1]};
    for (const auto& [space, matrix] :
         {std::pair{QVideoFrameFormat::ColorSpace_BT709, frame_io::YuvMatrix::Bt709},
          std::pair{QVideoFrameFormat::ColorSpace_BT2020, frame_io::YuvMatrix::Bt2020}}) {
        QVideoFrameFormat format(QSize(4, 2), PixelFormat::Format_NV12);
        format.setColorSpace(space);
        const QVideoFrame tagged = frameOf(format, [&uv](QVideoFrame& frame) {
            fillPlane(frame, 0, kY.data(), 4, 2);
            fillPlane(frame, 1, uv.data(), 4, 1);
        });
        const cv::Mat expected = frame_io::yuvToBgr({.layout = frame_io::YuvLayout::Nv12,
                                                     .size = {4, 2},
                                                     .data = {kY.data(), uv.data(), nullptr},
                                                     .stride = {4, 4, 0}},
                                                    matrix, frame_io::YuvRange::Video);
        EXPECT_EQ(cv::norm(convert(tagged), expected, cv::NORM_INF), 0.0) << static_cast<int>(matrix);
    }
}

// Red in the top-left corner of a 4×2 picture, the rest black.
cv::Mat redCorner() {
    cv::Mat picture(2, 4, CV_8UC3, cv::Scalar::all(0));
    picture.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 255);
    return picture;
}

bool isRed(const cv::Mat& image, int x, int y) { return image.at<cv::Vec3b>(y, x) == cv::Vec3b(0, 0, 255); }

const std::array<char, 4> kBgra{'B', 'G', 'R', 'A'};

// Rotation is applied, clockwise; the mirroring flags say how to show a frame, not how it was taken.
TEST(CameraFrame, RotatesButNeverMirrors) {
    QVideoFrame rotated = fourByteFrame(PixelFormat::Format_BGRA8888, kBgra, redCorner());
    setRotation90(rotated);
    EXPECT_EQ(document_lab::rotationOf(rotated), 90);
    const cv::Mat upright = convert(rotated);
    ASSERT_EQ(upright.size(), cv::Size(2, 4));
    EXPECT_TRUE(isRed(upright, 1, 0));

    QVideoFrame mirrored = fourByteFrame(PixelFormat::Format_BGRA8888, kBgra, redCorner());
    mirrored.setMirrored(true);
    EXPECT_TRUE(isRed(convert(mirrored), 0, 0));
}

#if defined(Q_OS_MACOS) && QT_VERSION < QT_VERSION_CHECK(6, 13, 0)
// darwin mirrors a front camera's pixels without saying so: the lab turns them back, once, whatever the
// flags.
TEST(CameraFrame, FrontCameraFlippedOnce) {
    const document_lab::CameraGeometry front{.frontCamera = true};
    EXPECT_TRUE(isRed(convert(fourByteFrame(PixelFormat::Format_BGRA8888, kBgra, redCorner()), front), 3, 0));

    QVideoFrame flagged = fourByteFrame(PixelFormat::Format_BGRA8888, kBgra, redCorner());
    flagged.setMirrored(true);
    EXPECT_TRUE(isRed(convert(flagged, front), 3, 0));
}
#endif

TEST(CameraFrame, BottomToTopIsFlipped) {
    const QVideoFrame frame =
        fourByteFrame(PixelFormat::Format_BGRA8888, kBgra, redCorner(), QVideoFrameFormat::BottomToTop);

    EXPECT_TRUE(isRed(convert(frame), 0, 1));
}

TEST(CameraFrame, P010IsSixteenBitAndTagged) {
    QVideoFrameFormat format(QSize(4, 2), PixelFormat::Format_P010);
    format.setColorTransfer(QVideoFrameFormat::ColorTransfer_STD_B67);
    format.setColorSpace(QVideoFrameFormat::ColorSpace_BT2020);
    const QVideoFrame frame = frameOf(format, [](QVideoFrame& frame) {
        std::array<std::uint16_t, 8> y{};
        y.fill(static_cast<std::uint16_t>(940 << 6));
        std::array<std::uint16_t, 4> uv{};
        uv.fill(static_cast<std::uint16_t>(512 << 6));
        fillPlane(frame, 0, reinterpret_cast<const std::uint8_t*>(y.data()), 8, 2);
        fillPlane(frame, 1, reinterpret_cast<const std::uint8_t*>(uv.data()), 8, 1);
    });
    std::string error;

    const std::optional<frame_io::Frame> converted = document_lab::toFrame(frame, {}, 0ms, error);

    ASSERT_TRUE(converted) << error;
    EXPECT_EQ(converted->image.type(), CV_16UC3);
    EXPECT_NEAR(converted->image.at<cv::Vec3w>(1, 2)[1], 60218, 64);
    EXPECT_EQ(converted->tags.transfer, visual_debug::Transfer::Hlg);
    EXPECT_EQ(converted->tags.primaries, visual_debug::Primaries::Bt2020);
    EXPECT_EQ(converted->tags.valueRange, (visual_debug::ValueRange{4100, 60218}));
}

TEST(CameraFrame, UnknownFormatIsNamed) {
    const QVideoFrame frame(QVideoFrameFormat(QSize(4, 2), PixelFormat::Format_AYUV));
    std::string error;

    EXPECT_FALSE(document_lab::toFrame(frame, {}, 0ms, error));

    EXPECT_NE(error.find("AYUV"), std::string::npos) << error;
}

} // namespace
