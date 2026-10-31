#include "viewer_core/display_converter.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace viewer_core {
namespace {

bool isFloatingPoint(int depth) { return depth == CV_32F || depth == CV_64F; }

void checkSupported(const cv::Mat& image) {
    if (image.empty() || image.dims != 2 || image.depth() > CV_64F || image.channels() > 4) {
        throw std::invalid_argument(
            "toDisplayImage: expected a 2-D image of depth 8U to 64F with 1-4 channels");
    }
}

// 255 where the value is finite. NaN fails every comparison, so one "|v| <= max" test catches NaN and ±Inf.
cv::Mat finiteMask(const cv::Mat& values) {
    const double largest =
        values.depth() == CV_32F ? std::numeric_limits<float>::max() : std::numeric_limits<double>::max();
    const cv::Mat magnitudes = cv::abs(values);
    cv::Mat mask;
    cv::compare(magnitudes, largest, mask, cv::CMP_LE);
    return mask;
}

// An image's channels side by side in one plane, so one range covers them all and they keep their relative
// brightness. For floating point, NaN and infinities are set to 0, away from the arithmetic, and `finite`
// marks the others; integer depths have neither, and leave `finite` empty.
struct Plane {
    cv::Mat values;
    cv::Mat finite;
};

Plane planeOf(const cv::Mat& image) {
    Plane plane{image.reshape(1), {}};
    if (isFloatingPoint(image.depth())) {
        plane.finite = finiteMask(plane.values);
        plane.values = plane.values.clone();
        plane.values.setTo(0, ~plane.finite);
    }
    return plane;
}

DisplayRange finiteRange(const Plane& plane) {
    DisplayRange range;
    cv::minMaxIdx(plane.values, &range.min, &range.max, nullptr, nullptr, plane.finite);
    return range;
}

// range.min becomes 0 and range.max 255, with values outside clipped. An empty range is black, and so are
// non-finite values. Half-spans, so that a range as wide as -1e308 to 1e308 does not overflow to infinity.
cv::Mat toEightBit(const Plane& plane, const DisplayRange& range, int channels) {
    cv::Mat result;
    const double halfSpan = range.max / 2.0 - range.min / 2.0;
    if (halfSpan > 0.0) {
        const double scale = 127.5 / halfSpan;
        // Straight to 8 bits, OpenCV scales in single precision, where a scale outside float's range would
        // become 0 or infinity: such a scale goes through double precision first.
        if (scale >= std::numeric_limits<float>::min() && scale <= std::numeric_limits<float>::max()) {
            plane.values.convertTo(result, CV_8U, scale, -range.min * scale);
        } else {
            cv::Mat scaled;
            plane.values.convertTo(scaled, CV_64F, scale, -range.min * scale);
            scaled.convertTo(result, CV_8U);
        }
    } else {
        result = cv::Mat::zeros(plane.values.size(), CV_8U);
    }
    if (!plane.finite.empty()) {
        result.setTo(0, ~plane.finite);
    }
    return result.reshape(channels);
}

cv::Mat toRgba(const cv::Mat& eightBit) {
    cv::Mat display;
    switch (eightBit.channels()) {
    case 1:
        cv::cvtColor(eightBit, display, cv::COLOR_GRAY2RGBA);
        break;
    case 2: {
        // Two channels are typically a vector field such as optical flow: show them as red and green.
        std::vector<cv::Mat> planes;
        cv::split(eightBit, planes);
        planes.push_back(cv::Mat::zeros(eightBit.size(), CV_8UC1));
        planes.push_back(cv::Mat(eightBit.size(), CV_8UC1, cv::Scalar(255)));
        cv::merge(planes, display);
        break;
    }
    case 3:
        cv::cvtColor(eightBit, display, cv::COLOR_BGR2RGBA);
        break;
    default:
        cv::cvtColor(eightBit, display, cv::COLOR_BGRA2RGBA);
        break;
    }
    return display;
}

// `finiteShare` is, per pixel, the share of finite values it stands for, 0 to 255 (the finite mask at full
// size). Every map gives 0 a colour of its own, so the colour is made from the finite values alone and then
// darkened by the share of the others: they stay black, as in grey.
cv::Mat colourMapped(cv::Mat eightBit, ColourMap colours, const cv::Mat& finiteShare) {
    if (!finiteShare.empty()) {
        cv::divide(eightBit, finiteShare, eightBit, 255.0); // the mean of the finite values; 0 where none
    }
    cv::Mat bgr;
    cv::applyColorMap(eightBit, bgr, colours == ColourMap::Turbo ? cv::COLORMAP_TURBO : cv::COLORMAP_VIRIDIS);
    if (!finiteShare.empty()) {
        cv::Mat share;
        cv::cvtColor(finiteShare, share, cv::COLOR_GRAY2BGR);
        cv::multiply(bgr, share, bgr, 1.0 / 255.0);
    }
    cv::Mat display;
    cv::cvtColor(bgr, display, cv::COLOR_BGR2RGBA);
    return display;
}

// Area averaging by the largest whole factor, which OpenCV does fast, then linear for the rest, under 2,
// where linear loses nothing. Area averaging at any other factor costs about 50 times as much: 37 ms for a 4K
// frame on one core, measured on an M4. The fast path needs the factor exact, so the last few columns and
// rows that do not make a whole block, under one pixel of the result, are left out.
cv::Mat shrunk(const cv::Mat& image, cv::Size size) {
    const int factor = std::max(1, std::min(image.cols / size.width, image.rows / size.height));
    cv::Mat averaged = image;
    if (factor > 1) {
        const cv::Size blocks(image.cols / factor, image.rows / factor);
        cv::resize(image(cv::Rect(0, 0, blocks.width * factor, blocks.height * factor)), averaged, blocks, 0,
                   0, cv::INTER_AREA);
    }
    if (averaged.size() == size) {
        return averaged;
    }
    cv::Mat small;
    cv::resize(averaged, small, size, 0, 0, cv::INTER_LINEAR);
    return small;
}

} // namespace

cv::Mat toDisplayImage(const cv::Mat& image) { return toDisplayImage(image, DisplaySettings{}); }

cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings) {
    checkSupported(image);
    return toDisplayImage(image, settings, image.size());
}

cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings, cv::Size size) {
    checkSupported(image);
    const cv::Size drawn(std::clamp(size.width, 1, image.cols), std::clamp(size.height, 1, image.rows));
    const bool coloured = image.channels() == 1 && settings.colours != ColourMap::Grey;
    cv::Mat eightBit;
    cv::Mat finite;
    if (settings.mode == RangeMode::Auto && image.depth() == CV_8U) {
        eightBit = image;
    } else {
        const Plane plane = planeOf(image);
        const DisplayRange range = settings.mode == RangeMode::Auto ? finiteRange(plane) : settings.range;
        eightBit = toEightBit(plane, range, image.channels());
        finite = plane.finite;
    }
    // 8-bit planes shrink the same for every depth, 8S and 32S included, which OpenCV cannot average.
    if (drawn != image.size()) {
        eightBit = shrunk(eightBit, drawn);
        if (coloured && !finite.empty()) {
            finite = shrunk(finite, drawn);
        }
    }
    if (coloured) {
        return colourMapped(eightBit, settings.colours, finite);
    }
    return toRgba(eightBit);
}

cv::Size sizeWithin(cv::Size image, cv::Size bounds) {
    const double scale = std::min({1.0, static_cast<double>(bounds.width) / std::max(1, image.width),
                                   static_cast<double>(bounds.height) / std::max(1, image.height)});
    return {std::max(1, static_cast<int>(std::lround(image.width * scale))),
            std::max(1, static_cast<int>(std::lround(image.height * scale)))};
}

DisplayRange autoRange(const cv::Mat& image) {
    checkSupported(image);
    if (image.depth() == CV_8U) {
        return {0.0, 255.0};
    }
    return finiteRange(planeOf(image));
}

std::size_t displayBytes(const cv::Mat& image) { return image.total() * 4; } // 8-bit RGBA

} // namespace viewer_core
