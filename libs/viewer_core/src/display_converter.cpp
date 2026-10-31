#include "viewer_core/display_converter.h"

#include "viewer_core/hdr_tone_map.h"

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
        // Straight to 8 bits, OpenCV works in single precision: a double beyond float's range becomes
        // infinity, and so does a scale beyond it. Doubles, and such scales, are scaled in double precision
        // first and clipped there, so only 0 to 255 reaches the conversion.
        if (plane.values.depth() != CV_64F && scale >= std::numeric_limits<float>::min() &&
            scale <= std::numeric_limits<float>::max()) {
            plane.values.convertTo(result, CV_8U, scale, -range.min * scale);
        } else {
            cv::Mat scaled;
            plane.values.convertTo(scaled, CV_64F, scale, -range.min * scale);
            cv::max(scaled, 0.0, scaled);
            cv::min(scaled, 255.0, scaled);
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

// Area averaging by the largest whole factor across and down, which OpenCV does fast, then linear for the
// rest, under 2, where linear loses nothing. Area averaging at any other factor costs about 50 times as much:
// 37 ms for a 4K frame on one core, measured on an M4. The fast path needs the factors exact, so the last few
// columns and rows that do not make a whole block, under one pixel of the result, are left out. Each side has
// its own factor, so a long thin image is averaged along its length however short its other side.
cv::Mat shrunk(const cv::Mat& image, cv::Size size) {
    const int across = std::max(1, image.cols / size.width);
    const int down = std::max(1, image.rows / size.height);
    cv::Mat averaged = image;
    if (across > 1 || down > 1) {
        const cv::Size blocks(image.cols / across, image.rows / down);
        cv::resize(image(cv::Rect(0, 0, blocks.width * across, blocks.height * down)), averaged, blocks, 0, 0,
                   cv::INTER_AREA);
    }
    if (averaged.size() == size) {
        return averaged;
    }
    cv::Mat small;
    cv::resize(averaged, small, size, 0, 0, cv::INTER_LINEAR);
    return small;
}

// The HDR range tone-maps at the drawn size, so the signal is shrunk first: tone mapping is not linear, but
// a 4K frame then costs no more than the window's pixels. Floating point has its non-finite values set to 0
// first, and `finite` marks the others at that size (for a colour map; one channel only). A fourth channel is
// alpha, mapped linearly from the signal range. HLG and PQ imply BT.2020 primaries when none are given.
cv::Mat toneMappedAtSize(const cv::Mat& image, const ImageTags& tags, cv::Size drawn, cv::Mat& finite) {
    const DisplayRange range = hdrSignalRange(image.depth(), tags);
    cv::Mat signal = image;
    if (isFloatingPoint(image.depth())) {
        const Plane plane = planeOf(image);
        signal = plane.values.reshape(image.channels());
        if (image.channels() == 1) {
            finite = plane.finite;
        }
    } else if (image.depth() == CV_8S || image.depth() == CV_32S) {
        image.convertTo(signal, CV_32F); // OpenCV cannot average them
    }
    if (drawn != image.size()) {
        signal = shrunk(signal, drawn);
        if (!finite.empty()) {
            finite = shrunk(finite, drawn);
        }
    }
    const Primaries primaries = tags.primaries.value_or(Primaries::Bt2020);
    if (signal.channels() != 4) {
        return toneMapToSdr(signal, *tags.transfer, primaries, range);
    }
    std::vector<cv::Mat> planes;
    cv::split(signal, planes);
    cv::Mat alpha;
    const double scale = 255.0 / (range.max - range.min);
    planes[3].convertTo(alpha, CV_8U, scale, -range.min * scale);
    planes.pop_back();
    cv::Mat colour;
    cv::merge(planes, colour);
    cv::split(toneMapToSdr(colour, *tags.transfer, primaries, range), planes);
    planes.push_back(alpha);
    cv::Mat mapped;
    cv::merge(planes, mapped);
    return mapped;
}

} // namespace

cv::Mat toDisplayImage(const cv::Mat& image) { return toDisplayImage(image, DisplaySettings{}); }

cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings) {
    return toDisplayImage(image, settings, DisplayOptions{});
}

cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings, cv::Size size) {
    // At least 1 on each side, so a zero size still means one pixel here, not the image's own size.
    return toDisplayImage(image, settings,
                          DisplayOptions{.size = {std::max(size.width, 1), std::max(size.height, 1)}});
}

cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings, const DisplayOptions& options) {
    checkSupported(image);
    const cv::Size drawn = options.size == cv::Size{}
                               ? image.size()
                               : cv::Size(std::clamp(options.size.width, 1, image.cols),
                                          std::clamp(options.size.height, 1, image.rows));
    const bool coloured = image.channels() == 1 && settings.colours != ColourMap::Grey;
    // Hdr shows what it cannot tone-map, an image without an HDR tag or of two channels, as Auto.
    const bool hdr = settings.mode == RangeMode::Hdr && options.tags.isHdr() && image.channels() != 2;
    const bool automatic = settings.mode == RangeMode::Auto || (settings.mode == RangeMode::Hdr && !hdr);
    const bool hinted = automatic && options.tags.hasValueRange();
    cv::Mat eightBit;
    cv::Mat finite;
    if (hdr) {
        eightBit = toneMappedAtSize(image, options.tags, drawn, finite);
    } else {
        if (automatic && !hinted && image.depth() == CV_8U) {
            eightBit = image;
        } else {
            const Plane plane = planeOf(image);
            const DisplayRange range =
                hinted ? options.tags.valueRange : (automatic ? finiteRange(plane) : settings.range);
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

DisplayRange autoRange(const cv::Mat& image, const ImageTags& tags) {
    checkSupported(image);
    if (tags.hasValueRange()) {
        return tags.valueRange;
    }
    if (image.depth() == CV_8U) {
        return {0.0, 255.0};
    }
    return finiteRange(planeOf(image));
}

std::size_t displayBytes(const cv::Mat& image) { return image.total() * 4; } // 8-bit RGBA

} // namespace viewer_core
