#include "camera_frame.h"

#include <frame_io/yuv.h>

#include <QString>
#include <QVideoFrameFormat>
#include <QtGlobal>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace document_lab {

namespace {

using PixelFormat = QVideoFrameFormat::PixelFormat;

std::optional<frame_io::YuvLayout> yuvLayoutOf(PixelFormat format) {
    switch (format) {
    case PixelFormat::Format_NV12:
        return frame_io::YuvLayout::Nv12;
    case PixelFormat::Format_NV21:
        return frame_io::YuvLayout::Nv21;
    case PixelFormat::Format_YUYV:
        return frame_io::YuvLayout::Yuyv;
    case PixelFormat::Format_UYVY:
        return frame_io::YuvLayout::Uyvy;
    case PixelFormat::Format_YUV420P:
        return frame_io::YuvLayout::I420;
    case PixelFormat::Format_YV12:
        return frame_io::YuvLayout::Yv12;
    default:
        return std::nullopt;
    }
}

// Where B, G and R are among a four-byte format's bytes, in memory order. A premultiplied frame from a camera
// is opaque, so it is read as a plain one.
std::optional<std::array<int, 3>> bgrBytesOf(PixelFormat format) {
    switch (format) {
    case PixelFormat::Format_BGRA8888:
    case PixelFormat::Format_BGRA8888_Premultiplied:
    case PixelFormat::Format_BGRX8888:
        return std::array{0, 1, 2};
    case PixelFormat::Format_RGBA8888:
    case PixelFormat::Format_RGBX8888:
        return std::array{2, 1, 0};
    case PixelFormat::Format_ARGB8888:
    case PixelFormat::Format_ARGB8888_Premultiplied:
    case PixelFormat::Format_XRGB8888:
        return std::array{3, 2, 1};
    case PixelFormat::Format_ABGR8888:
    case PixelFormat::Format_XBGR8888:
        return std::array{1, 2, 3};
    default:
        return std::nullopt;
    }
}

bool isConvertible(PixelFormat format) {
    return yuvLayoutOf(format) || bgrBytesOf(format) || format == PixelFormat::Format_P010 ||
           format == PixelFormat::Format_Jpeg;
}

// As Qt treats them: BT.709 or BT.2020 when the format says so, BT.601 otherwise, Undefined included.
frame_io::YuvMatrix matrixOf(const QVideoFrameFormat& format) {
    switch (format.colorSpace()) {
    case QVideoFrameFormat::ColorSpace_BT709:
        return frame_io::YuvMatrix::Bt709;
    case QVideoFrameFormat::ColorSpace_BT2020:
        return frame_io::YuvMatrix::Bt2020;
    default:
        return frame_io::YuvMatrix::Bt601;
    }
}

frame_io::YuvRange rangeOf(const QVideoFrameFormat& format) {
    return format.colorRange() == QVideoFrameFormat::ColorRange_Full ? frame_io::YuvRange::Full
                                                                     : frame_io::YuvRange::Video;
}

std::optional<visual_debug::Transfer> transferOf(const QVideoFrameFormat& format) {
    switch (format.colorTransfer()) {
    case QVideoFrameFormat::ColorTransfer_ST2084:
        return visual_debug::Transfer::Pq;
    case QVideoFrameFormat::ColorTransfer_STD_B67:
        return visual_debug::Transfer::Hlg;
    default:
        return std::nullopt;
    }
}

std::string nameOf(PixelFormat format) {
    return QVideoFrameFormat::pixelFormatToString(format).toStdString();
}

// cv::Mat wraps memory it does not own as non-const; these Mats are only read.
cv::Mat wrap(cv::Size size, int type, const uchar* data, int stride) {
    return cv::Mat(size, type, const_cast<uchar*>(data), static_cast<std::size_t>(stride));
}

// The mapped frame's pixels as BGR, a new buffer; P010's tags into `tags`.
cv::Mat convert(const QVideoFrame& mapped, const QVideoFrameFormat& format,
                visual_debug::ImageDescription& tags) {
    const PixelFormat pixelFormat = format.pixelFormat();
    const cv::Size size(mapped.width(), mapped.height());
    if (const std::optional<frame_io::YuvLayout> layout = yuvLayoutOf(pixelFormat)) {
        frame_io::YuvPlanes planes{.layout = *layout, .size = size, .data = {}, .stride = {}};
        for (int plane = 0; plane < std::min(mapped.planeCount(), 3); ++plane) {
            planes.data[static_cast<std::size_t>(plane)] = mapped.bits(plane);
            planes.stride[static_cast<std::size_t>(plane)] = mapped.bytesPerLine(plane);
        }
        return frame_io::yuvToBgr(planes, matrixOf(format), rangeOf(format));
    }
    if (const std::optional<std::array<int, 3>> bytes = bgrBytesOf(pixelFormat)) {
        if (!mapped.bits(0) || mapped.bytesPerLine(0) < 4 * size.width || size.empty()) {
            return {};
        }
        const cv::Mat fourBytes = wrap(size, CV_8UC4, mapped.bits(0), mapped.bytesPerLine(0));
        cv::Mat bgr(size, CV_8UC3);
        const std::array<int, 6> fromTo{(*bytes)[0], 0, (*bytes)[1], 1, (*bytes)[2], 2};
        cv::mixChannels(&fourBytes, 1, &bgr, 1, fromTo.data(), 3);
        return bgr;
    }
    if (pixelFormat == PixelFormat::Format_P010) {
        tags.valueRange = frame_io::p010ValueRange(rangeOf(format));
        tags.transfer = transferOf(format);
        if (format.colorSpace() == QVideoFrameFormat::ColorSpace_BT2020) {
            tags.primaries = visual_debug::Primaries::Bt2020;
        }
        return frame_io::p010ToBgr16(mapped.bits(0), mapped.bytesPerLine(0), mapped.bits(1),
                                     mapped.bytesPerLine(1), size, matrixOf(format), rangeOf(format));
    }
    if (pixelFormat == PixelFormat::Format_Jpeg && mapped.bits(0) && mapped.mappedBytes(0) > 0) {
        return cv::imdecode(wrap({mapped.mappedBytes(0), 1}, CV_8UC1, mapped.bits(0), mapped.mappedBytes(0)),
                            cv::IMREAD_COLOR);
    }
    return {};
}

// Upright and unmirrored: rows stored bottom to top turned over, darwin's front-camera mirroring undone,
// then the rotation applied. The mirroring flags say how a frame should be shown, so they are left alone.
cv::Mat upright(cv::Mat image, const QVideoFrame& frame, const QVideoFrameFormat& format,
                CameraGeometry geometry) {
    cv::Mat turned;
    if (format.scanLineDirection() == QVideoFrameFormat::BottomToTop) {
        cv::flip(image, turned, 0);
        image = std::move(turned);
    }
#if defined(Q_OS_MACOS) && QT_VERSION < QT_VERSION_CHECK(6, 13, 0)
    // The darwin backend of these versions, whose code was read, mirrors a front camera's pixels unflagged.
    if (geometry.frontCamera) {
        cv::flip(image, turned, 1);
        image = std::move(turned);
    }
#else
    static_cast<void>(geometry);
#endif
    switch (rotationOf(frame)) {
    case 90:
        cv::rotate(image, turned, cv::ROTATE_90_CLOCKWISE);
        return turned;
    case 180:
        cv::rotate(image, turned, cv::ROTATE_180);
        return turned;
    case 270:
        cv::rotate(image, turned, cv::ROTATE_90_COUNTERCLOCKWISE);
        return turned;
    default:
        return image;
    }
}

} // namespace

int rotationOf(const QVideoFrame& frame) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    int degrees = static_cast<int>(frame.rotation());
#else
    int degrees = static_cast<int>(frame.rotationAngle());
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    degrees += static_cast<int>(frame.surfaceFormat().rotation());
#endif
    return (degrees % 360 + 360) % 360;
}

std::optional<frame_io::Frame> toFrame(const QVideoFrame& frame, CameraGeometry geometry,
                                       std::chrono::nanoseconds at, std::string& error) {
    const PixelFormat pixelFormat = frame.pixelFormat();
    if (!isConvertible(pixelFormat)) {
        error = "the camera's " + nameOf(pixelFormat) + " frames cannot be converted";
        return std::nullopt;
    }
    QVideoFrame mapped = frame; // shares the buffer: map() is not const
    if (!mapped.map(QVideoFrame::ReadOnly)) {
        error = "a " + nameOf(pixelFormat) + " camera frame could not be read";
        return std::nullopt;
    }
    const QVideoFrameFormat format = mapped.surfaceFormat();
    visual_debug::ImageDescription tags;
    cv::Mat image = convert(mapped, format, tags);
    mapped.unmap();
    if (image.empty()) {
        error = "a " + nameOf(pixelFormat) + " camera frame of " + std::to_string(frame.width()) + "×" +
                std::to_string(frame.height()) + " could not be converted";
        return std::nullopt;
    }
    image = upright(std::move(image), frame, format, geometry);
    tags = frame_io::tagsFor(image, std::move(tags));
    return frame_io::Frame{.image = std::move(image), .number = 0, .at = at, .tags = std::move(tags)};
}

} // namespace document_lab
