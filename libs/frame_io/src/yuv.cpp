#include "frame_io/yuv.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace frame_io {

namespace {

// How much each normalised colour difference adds to R, G and B: R = Y' + rCr·Cr', G = Y' + gCb·Cb' +
// gCr·Cr', B = Y' + bCb·Cb'.
struct Coefficients {
    double rCr;
    double gCb;
    double gCr;
    double bCb;
};

Coefficients coefficientsOf(YuvMatrix matrix) {
    if (matrix == YuvMatrix::Bt709) {
        return {.rCr = 1.5748, .gCb = -0.187324, .gCr = -0.468124, .bCb = 1.8556};
    }
    return {.rCr = 1.402, .gCb = -0.344136, .gCr = -0.714136, .bCb = 1.772};
}

// Where the input's codes sit: Y' = (Y − lumaBlack) / lumaSpan, C' = (C − chromaMiddle) / chromaSpan.
struct InputLevels {
    double lumaBlack;
    double lumaSpan;
    double chromaMiddle;
    double chromaSpan;
};

// Where the output puts a normalised value v: black + v × span.
struct OutputLevels {
    double black;
    double span;
};

// The 3×4 matrix that takes (Y, U, V, 1) to (B, G, R) in output units, for cv::transform, which saturates.
cv::Matx34d matrixFor(YuvMatrix matrix, InputLevels in, OutputLevels out) {
    const Coefficients k = coefficientsOf(matrix);
    const double cb[3] = {k.bCb, k.gCb, 0.0};
    const double cr[3] = {0.0, k.gCr, k.rCr};
    cv::Matx34d m;
    for (int row = 0; row < 3; ++row) {
        m(row, 0) = out.span / in.lumaSpan;
        m(row, 1) = out.span * cb[row] / in.chromaSpan;
        m(row, 2) = out.span * cr[row] / in.chromaSpan;
        m(row, 3) = out.black + out.span * (-in.lumaBlack / in.lumaSpan -
                                            (cb[row] + cr[row]) * in.chromaMiddle / in.chromaSpan);
    }
    return m;
}

bool halvesHeight(YuvLayout layout) { return layout != YuvLayout::Yuyv && layout != YuvLayout::Uyvy; }

bool isWellFormed(const YuvPlanes& planes) {
    const int width = planes.size.width;
    const int height = planes.size.height;
    if (width <= 0 || height <= 0 || width % 2 != 0 || (halvesHeight(planes.layout) && height % 2 != 0)) {
        return false;
    }
    switch (planes.layout) {
    case YuvLayout::Nv12:
    case YuvLayout::Nv21:
        return planes.data[0] && planes.data[1] && planes.stride[0] >= width && planes.stride[1] >= width;
    case YuvLayout::Yuyv:
    case YuvLayout::Uyvy:
        return planes.data[0] && planes.stride[0] >= 2 * width;
    case YuvLayout::I420:
    case YuvLayout::Yv12:
        return planes.data[0] && planes.data[1] && planes.data[2] && planes.stride[0] >= width &&
               planes.stride[1] >= width / 2 && planes.stride[2] >= width / 2;
    }
    return false;
}

// cv::Mat wraps memory it does not own as non-const; these Mats are only ever read.
cv::Mat wrap(cv::Size size, int type, const std::uint8_t* data, int stride) {
    return cv::Mat(size, type, const_cast<std::uint8_t*>(data), static_cast<std::size_t>(stride));
}

// I420 and YV12 as OpenCV wants them: the three planes one after the other in one (h·3/2)×w buffer.
cv::Mat packedPlanar(const YuvPlanes& planes) {
    const int width = planes.size.width;
    const int height = planes.size.height;
    cv::Mat packed(height * 3 / 2, width, CV_8UC1);
    std::uint8_t* out = packed.data;
    for (int row = 0; row < height; ++row, out += width) {
        std::memcpy(out, planes.data[0] + static_cast<std::ptrdiff_t>(row) * planes.stride[0],
                    static_cast<std::size_t>(width));
    }
    for (int plane = 1; plane <= 2; ++plane) {
        for (int row = 0; row < height / 2; ++row, out += width / 2) {
            std::memcpy(out, planes.data[plane] + static_cast<std::ptrdiff_t>(row) * planes.stride[plane],
                        static_cast<std::size_t>(width / 2));
        }
    }
    return packed;
}

// BT.601 video range, the one OpenCV converts itself.
cv::Mat openCvConversion(const YuvPlanes& planes) {
    const cv::Size size = planes.size;
    const cv::Size half(size.width / 2, size.height / 2);
    cv::Mat bgr;
    switch (planes.layout) {
    case YuvLayout::Nv12:
    case YuvLayout::Nv21:
        cv::cvtColorTwoPlane(wrap(size, CV_8UC1, planes.data[0], planes.stride[0]),
                             wrap(half, CV_8UC2, planes.data[1], planes.stride[1]), bgr,
                             planes.layout == YuvLayout::Nv12 ? cv::COLOR_YUV2BGR_NV12
                                                              : cv::COLOR_YUV2BGR_NV21);
        break;
    case YuvLayout::Yuyv:
    case YuvLayout::Uyvy:
        cv::cvtColor(wrap(size, CV_8UC2, planes.data[0], planes.stride[0]), bgr,
                     planes.layout == YuvLayout::Yuyv ? cv::COLOR_YUV2BGR_YUYV : cv::COLOR_YUV2BGR_UYVY);
        break;
    case YuvLayout::I420:
    case YuvLayout::Yv12:
        cv::cvtColor(packedPlanar(planes), bgr,
                     planes.layout == YuvLayout::I420 ? cv::COLOR_YUV2BGR_I420 : cv::COLOR_YUV2BGR_YV12);
        break;
    }
    return bgr;
}

// Y, U and V at full size, the chroma repeated over the pixels that share it, as OpenCV's conversions do.
cv::Mat fullSizeYuv(const YuvPlanes& planes) {
    const cv::Size size = planes.size;
    const cv::Size half(size.width / 2, size.height / 2);
    cv::Mat luma;
    cv::Mat u;
    cv::Mat v;
    switch (planes.layout) {
    case YuvLayout::Nv12:
    case YuvLayout::Nv21: {
        luma = wrap(size, CV_8UC1, planes.data[0], planes.stride[0]);
        std::vector<cv::Mat> chroma;
        cv::split(wrap(half, CV_8UC2, planes.data[1], planes.stride[1]), chroma);
        u = chroma[planes.layout == YuvLayout::Nv12 ? 0 : 1];
        v = chroma[planes.layout == YuvLayout::Nv12 ? 1 : 0];
        break;
    }
    case YuvLayout::Yuyv:
    case YuvLayout::Uyvy: {
        std::vector<cv::Mat> bytes;
        cv::split(wrap({size.width / 2, size.height}, CV_8UC4, planes.data[0], planes.stride[0]), bytes);
        const bool yFirst = planes.layout == YuvLayout::Yuyv;
        cv::Mat pairs;
        cv::merge(std::vector<cv::Mat>{bytes[yFirst ? 0 : 1], bytes[yFirst ? 2 : 3]}, pairs);
        luma = pairs.reshape(1, size.height);
        u = bytes[yFirst ? 1 : 0];
        v = bytes[yFirst ? 3 : 2];
        break;
    }
    case YuvLayout::I420:
    case YuvLayout::Yv12: {
        luma = wrap(size, CV_8UC1, planes.data[0], planes.stride[0]);
        const cv::Mat second = wrap(half, CV_8UC1, planes.data[1], planes.stride[1]);
        const cv::Mat third = wrap(half, CV_8UC1, planes.data[2], planes.stride[2]);
        u = planes.layout == YuvLayout::I420 ? second : third;
        v = planes.layout == YuvLayout::I420 ? third : second;
        break;
    }
    }
    cv::resize(u, u, size, 0.0, 0.0, cv::INTER_NEAREST);
    cv::resize(v, v, size, 0.0, 0.0, cv::INTER_NEAREST);
    cv::Mat yuv;
    cv::merge(std::vector<cv::Mat>{luma, u, v}, yuv);
    return yuv;
}

constexpr double kTenBitToSixteen = 65535.0 / 1023.0;

} // namespace

cv::Mat yuvToBgr(const YuvPlanes& planes, YuvMatrix matrix, YuvRange range) {
    if (!isWellFormed(planes)) {
        return {};
    }
    if (matrix == YuvMatrix::Bt601 && range == YuvRange::Video) {
        return openCvConversion(planes);
    }
    const InputLevels in = range == YuvRange::Video ? InputLevels{16.0, 219.0, 128.0, 224.0}
                                                    : InputLevels{0.0, 255.0, 128.0, 255.0};
    cv::Mat bgr;
    cv::transform(fullSizeYuv(planes), bgr, matrixFor(matrix, in, {.black = 0.0, .span = 255.0}));
    return bgr;
}

cv::Mat p010ToBgr16(const std::uint8_t* y, int yStride, const std::uint8_t* uv, int uvStride, cv::Size size,
                    YuvMatrix matrix, YuvRange range) {
    const int width = size.width;
    const int height = size.height;
    if (!y || !uv || width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0 || yStride < 2 * width ||
        uvStride < 2 * width || yStride % 2 != 0 || uvStride % 2 != 0) {
        return {};
    }
    const cv::Mat luma = wrap(size, CV_16UC1, y, yStride);
    std::vector<cv::Mat> chroma;
    cv::split(wrap({width / 2, height / 2}, CV_16UC2, uv, uvStride), chroma);
    for (cv::Mat& plane : chroma) {
        cv::resize(plane, plane, size, 0.0, 0.0, cv::INTER_NEAREST);
    }
    cv::Mat yuv;
    cv::merge(std::vector<cv::Mat>{luma, chroma[0], chroma[1]}, yuv);

    // The 10-bit codes stay in the words' top bits, so the levels are the codes × 64.
    const InputLevels in = range == YuvRange::Video
                               ? InputLevels{64.0 * 64, 876.0 * 64, 512.0 * 64, 896.0 * 64}
                               : InputLevels{0.0, 1023.0 * 64, 512.0 * 64, 1023.0 * 64};
    const OutputLevels out = range == YuvRange::Video ? OutputLevels{.black = 64.0 * kTenBitToSixteen,
                                                                     .span = 876.0 * kTenBitToSixteen}
                                                      : OutputLevels{.black = 0.0, .span = 65535.0};
    cv::Mat bgr;
    cv::transform(yuv, bgr, matrixFor(matrix, in, out));
    return bgr;
}

visual_debug::ValueRange p010ValueRange(YuvRange range) {
    if (range == YuvRange::Video) {
        return {.min = std::round(64.0 * kTenBitToSixteen), .max = std::round(940.0 * kTenBitToSixteen)};
    }
    return {.min = 0.0, .max = 65535.0};
}

} // namespace frame_io
