#include "viewer_core/display_converter.h"

#include "viewer_core/hdr_tone_map.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
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
        if (plane.values.depth() == CV_32F && scale > 1.0 && scale <= std::numeric_limits<float>::max()) {
            // A float far above the range would also become infinity when scaled, which x86's rounding
            // instruction turns into the integer minimum, so black. Clipped to the range first, in floats,
            // no value can overflow; with a scale of at most 1, none can anyway.
            constexpr double kFloatMax = std::numeric_limits<float>::max();
            cv::Mat clipped;
            cv::min(plane.values, std::min(range.max, kFloatMax), clipped);
            cv::max(clipped, std::max(range.min, -kFloatMax), clipped);
            clipped.convertTo(result, CV_8U, scale, -range.min * scale);
        } else if (plane.values.depth() != CV_64F && scale >= std::numeric_limits<float>::min() &&
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

// The HDR range tone-maps an integer signal at the drawn size, so it is shrunk first: tone mapping is not
// linear, but a 4K frame then costs no more than the window's pixels. Floating point has its non-finite
// values set to 0 first, and `finite` marks the others at the drawn size (for a colour map; one channel
// only). A fourth channel is alpha, mapped linearly from the signal range. HLG and PQ imply BT.2020 primaries
// when none are given.
cv::Mat toneMappedAtSize(const cv::Mat& image, const ImageTags& tags, cv::Size drawn, cv::Mat& finite) {
    const DisplayRange range = hdrSignalRange(image.depth(), tags);
    const bool shrinkFirst = !isFloatingPoint(image.depth()) && drawn != image.size();
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
    // An integer signal is shrunk first, so a 4K frame costs no more than the window's pixels. A
    // floating-point one is mapped whole and shrunk after: averaging values near its type's limits would
    // overflow to infinities, and mixing those makes NaN.
    if (shrinkFirst) {
        signal = shrunk(signal, drawn);
    }
    if (!finite.empty() && drawn != image.size()) {
        finite = shrunk(finite, drawn);
    }
    const Primaries primaries = tags.primaries.value_or(Primaries::Bt2020);
    cv::Mat mapped;
    if (signal.channels() != 4) {
        mapped = toneMapToSdr(signal, *tags.transfer, primaries, range);
    } else {
        std::vector<cv::Mat> planes;
        cv::split(signal, planes);
        cv::Mat alpha;
        const double scale = 127.5 / (range.max / 2 - range.min / 2); // in halves: no range overflows
        planes[3].convertTo(alpha, CV_8U, scale, -range.min * scale);
        planes.pop_back();
        cv::Mat colour;
        cv::merge(planes, colour);
        cv::split(toneMapToSdr(colour, *tags.transfer, primaries, range), planes);
        planes.push_back(alpha);
        cv::merge(planes, mapped);
    }
    return shrinkFirst || drawn == image.size() ? mapped : shrunk(mapped, drawn);
}

// The values a picture cuts off: at or below `bottom`, at or above `top`.
struct ClipRange {
    double bottom;
    double top;
};

// The HDR range cuts off below black and at the top of the scale, the camera's own clip: a super-white is
// shown, not cut off. Auto cuts off beyond its hint, or 0 and 255 for 8U; without a hint other depths are
// stretched from their own extremes, losing nothing. Hold and Fixed cut off beyond their own range, and an
// empty range shows black, marked nowhere.
std::optional<ClipRange> clippingRange(const cv::Mat& image, const DisplaySettings& settings,
                                       const ImageTags& tags, bool hdr, bool automatic) {
    if (hdr) {
        const DisplayRange signal = hdrSignalRange(image.depth(), tags);
        return ClipRange{signal.min, hdrScaleTop(image.depth(), signal)};
    }
    if (automatic) {
        if (tags.hasValueRange()) {
            return ClipRange{tags.valueRange.min, tags.valueRange.max};
        }
        return image.depth() == CV_8U ? std::optional(ClipRange{0.0, 255.0}) : std::nullopt;
    }
    if (!(settings.range.min < settings.range.max)) {
        return std::nullopt;
    }
    return ClipRange{settings.range.min, settings.range.max};
}

constexpr uchar kMarkedBelow = 1;
constexpr uchar kMarkedAbove = 2; // the larger, so that it wins wherever both meet

// Per pixel, at full size: above when any channel is at or above the top, else below when any is at or below
// the bottom. NaN fails every comparison, so it is never marked.
// A fourth channel is alpha, how opaque the pixel is, never a value that clips.
cv::Mat clippingMarks(const cv::Mat& image, const ClipRange& range) {
    const cv::Mat values = image.reshape(1);
    const int colourChannels = std::min(image.channels(), 3);
    const auto anyChannel = [&image, colourChannels](const cv::Mat& perValue) {
        if (image.channels() == 1) {
            return perValue;
        }
        std::vector<cv::Mat> channels;
        cv::split(perValue.reshape(image.channels()), channels);
        cv::Mat any = channels[0].clone();
        for (std::size_t i = 1; i < static_cast<std::size_t>(colourChannels); ++i) {
            cv::bitwise_or(any, channels[i], any);
        }
        return any;
    };
    cv::Mat above;
    cv::Mat below;
    cv::compare(values, range.top, above, cv::CMP_GE);
    cv::compare(values, range.bottom, below, cv::CMP_LE);
    cv::Mat marks = cv::Mat::zeros(image.size(), CV_8U);
    marks.setTo(kMarkedBelow, anyChannel(below));
    marks.setTo(kMarkedAbove, anyChannel(above));
    return marks;
}

// Each block of the picture takes its largest mark, so one clipped pixel still marks its block however small
// the picture is drawn; averaging would wash it out.
cv::Mat largestPerBlock(const cv::Mat& marks, cv::Size size) {
    if (marks.size() == size) {
        return marks;
    }
    cv::Mat result(size, CV_8U);
    cv::parallel_for_(cv::Range(0, size.height), [&](const cv::Range& rows) {
        for (int y = rows.start; y < rows.end; ++y) {
            const int top = y * marks.rows / size.height;
            const int bottom = std::max(top + 1, (y + 1) * marks.rows / size.height);
            auto* out = result.ptr<uchar>(y);
            for (int x = 0; x < size.width; ++x) {
                const int left = x * marks.cols / size.width;
                const int right = std::max(left + 1, (x + 1) * marks.cols / size.width);
                uchar largest = 0;
                for (int row = top; row < bottom && largest < kMarkedAbove; ++row) {
                    const auto* in = marks.ptr<uchar>(row);
                    for (int col = left; col < right; ++col) {
                        largest = std::max(largest, in[col]);
                    }
                }
                out[x] = largest;
            }
        }
    });
    return result;
}

cv::Scalar opaque(const cv::Vec3b& rgb) { return {double(rgb[0]), double(rgb[1]), double(rgb[2]), 255.0}; }

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
    cv::Mat display = coloured ? colourMapped(eightBit, settings.colours, finite) : toRgba(eightBit);
    if (options.showClipping) {
        if (const auto range = clippingRange(image, settings, options.tags, hdr, automatic)) {
            const cv::Mat marks = largestPerBlock(clippingMarks(image, *range), drawn);
            display.setTo(opaque(coloured ? kClippedBelowMapped : kClippedBelowGrey), marks == kMarkedBelow);
            display.setTo(opaque(kClippedAbove), marks == kMarkedAbove);
        }
    }
    return display;
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
