#include "viewer_core/histogram.h"

#include "viewer_core/display_converter.h"
#include "viewer_core/hdr_tone_map.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace viewer_core {
namespace {

bool isFloatingPoint(int depth) { return depth == CV_32F || depth == CV_64F; }

// The rule toDisplayImage applies, which keeps to itself.
void checkSupported(const cv::Mat& image) {
    if (image.empty() || image.dims != 2 || image.depth() > CV_64F || image.channels() > 4) {
        throw std::invalid_argument("histogram: expected a 2-D image of depth 8U to 64F with 1-4 channels");
    }
}

// Running minima and maxima of one stretch of rows; empty while lo > hi.
struct Extremes {
    double allLo = std::numeric_limits<double>::infinity();
    double allHi = -std::numeric_limits<double>::infinity();
    double colourLo = std::numeric_limits<double>::infinity();
    double colourHi = -std::numeric_limits<double>::infinity();
    std::size_t nonFinite = 0;

    void merge(const Extremes& other) {
        allLo = std::min(allLo, other.allLo);
        allHi = std::max(allHi, other.allHi);
        colourLo = std::min(colourLo, other.colourLo);
        colourHi = std::max(colourHi, other.colourHi);
        nonFinite += other.nonFinite;
    }
};

std::optional<DisplayRange> rangeOf(double lo, double hi) {
    return lo <= hi ? std::optional(DisplayRange{lo, hi}) : std::nullopt;
}

// Minimum, maximum and non-finite count of a run of values in their own type: no conversion per value. NaN
// fails every comparison, so `|v| <= max` is false for NaN and both infinities alike.
template <typename T> struct RunExtremes {
    T lo = std::numeric_limits<T>::max();
    T hi = std::numeric_limits<T>::lowest();
    std::size_t nonFinite = 0;
    bool any = false;

    void add(T value) {
        if constexpr (std::numeric_limits<T>::has_quiet_NaN) {
            if (!(std::abs(value) <= std::numeric_limits<T>::max())) {
                ++nonFinite;
                return;
            }
        }
        lo = std::min(lo, value);
        hi = std::max(hi, value);
        any = true;
    }
};

// One pass over rows [first, last), every channel, a fourth (alpha) kept apart from the colours.
template <typename T> Extremes extremesOf(const cv::Mat& image, int first, int last) {
    const int channels = image.channels();
    RunExtremes<T> colours;
    RunExtremes<T> alpha;
    for (int row = first; row < last; ++row) {
        const T* in = image.ptr<T>(row);
        if (channels < 4) {
            const int values = image.cols * channels;
            for (int i = 0; i < values; ++i) {
                colours.add(in[i]);
            }
        } else {
            for (int col = 0; col < image.cols; ++col, in += 4) {
                colours.add(in[0]);
                colours.add(in[1]);
                colours.add(in[2]);
                alpha.add(in[3]);
            }
        }
    }
    Extremes extremes;
    if (colours.any) {
        extremes.colourLo = extremes.allLo = static_cast<double>(colours.lo);
        extremes.colourHi = extremes.allHi = static_cast<double>(colours.hi);
    }
    if (alpha.any) {
        extremes.allLo = std::min(extremes.allLo, static_cast<double>(alpha.lo));
        extremes.allHi = std::max(extremes.allHi, static_cast<double>(alpha.hi));
    }
    extremes.nonFinite = colours.nonFinite;
    return extremes;
}

template <typename T> Extremes wholeFrameExtremes(const cv::Mat& image) {
    // Fixed pieces, each with its own slot, so the threads share nothing and the result never depends on how
    // OpenCV splits the work.
    const int pieces = std::min(image.rows, 64);
    std::vector<Extremes> partial(static_cast<std::size_t>(pieces));
    cv::parallel_for_(cv::Range(0, pieces), [&](const cv::Range& range) {
        for (int piece = range.start; piece < range.end; ++piece) {
            const int first = piece * image.rows / pieces;
            const int last = (piece + 1) * image.rows / pieces;
            partial[static_cast<std::size_t>(piece)] = extremesOf<T>(image, first, last);
        }
    });
    Extremes whole;
    for (const Extremes& piece : partial) {
        whole.merge(piece);
    }
    return whole;
}

Extremes extremesByDepth(const cv::Mat& image) {
    switch (image.depth()) {
    case CV_8U:
        return wholeFrameExtremes<std::uint8_t>(image);
    case CV_8S:
        return wholeFrameExtremes<std::int8_t>(image);
    case CV_16U:
        return wholeFrameExtremes<std::uint16_t>(image);
    case CV_16S:
        return wholeFrameExtremes<std::int16_t>(image);
    case CV_32S:
        return wholeFrameExtremes<std::int32_t>(image);
    case CV_32F:
        return wholeFrameExtremes<float>(image);
    default:
        return wholeFrameExtremes<double>(image);
    }
}

} // namespace

ValueExtent valueExtent(const cv::Mat& image) {
    checkSupported(image);
    // Integers of one to three channels have no NaN and no alpha: OpenCV's own pass, vectorised and threaded.
    // Floating point must check every value, and a fourth channel must be kept apart; NaN in an unmasked
    // minMaxIdx might give a finite but wrong extent on one platform and the right one on another.
    if (!isFloatingPoint(image.depth()) && image.channels() <= 3) {
        double lo = 0.0;
        double hi = 0.0;
        cv::minMaxIdx(image.reshape(1), &lo, &hi);
        return {.all = DisplayRange{lo, hi}, .colours = DisplayRange{lo, hi}, .nonFinite = 0};
    }
    const Extremes extremes = extremesByDepth(image);
    return {.all = rangeOf(extremes.allLo, extremes.allHi),
            .colours = rangeOf(extremes.colourLo, extremes.colourHi),
            .nonFinite = extremes.nonFinite};
}

std::optional<RangeMarkers> rangeMarkers(int depth, int channels, const DisplaySettings& settings,
                                         const ImageTags& tags, const ValueExtent& extent) {
    if (showsHdr(settings, tags, channels)) {
        const DisplayRange range = hdrBlackAndWhite(depth, settings, tags);
        return RangeMarkers{.range = range, .scaleTop = hdrScaleTop(depth, range), .hdr = true};
    }
    switch (settings.mode) {
    case RangeMode::Hold:
    case RangeMode::Fixed:
        return RangeMarkers{.range = settings.range};
    case RangeMode::Auto:
    case RangeMode::Hdr: // shown as Auto
        break;
    }
    // Auto's range, as autoRange takes it.
    if (tags.hasValueRange()) {
        return RangeMarkers{.range = tags.valueRange};
    }
    if (depth == CV_8U) {
        return RangeMarkers{.range = {0.0, 255.0}};
    }
    if (extent.all) {
        return RangeMarkers{.range = *extent.all};
    }
    return std::nullopt;
}

namespace {

constexpr double kLargest = std::numeric_limits<double>::max();

// The type's limits, for the integer depths.
DisplayRange integerLimits(int depth) {
    switch (depth) {
    case CV_8U:
        return {0.0, 255.0};
    case CV_8S:
        return {-128.0, 127.0};
    case CV_16U:
        return {0.0, 65535.0};
    case CV_16S:
        return {-32768.0, 32767.0};
    default:
        return {static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                static_cast<double>(std::numeric_limits<std::int32_t>::max())};
    }
}

// Half the axis's span: a range as wide as -1e308 to 1e308 is not a double, its half is.
double halfSpan(const DisplayRange& range) { return range.max / 2 - range.min / 2; }

// The smallest step whose grid of every step-th pixel across and down has at most `cap` pixels.
int samplingStep(int rows, int cols, std::size_t cap) {
    const auto pixelsAt = [rows, cols](int step) {
        return static_cast<std::size_t>((rows + step - 1) / step) *
               static_cast<std::size_t>((cols + step - 1) / step);
    };
    const double ratio = static_cast<double>(rows) * static_cast<double>(cols) / static_cast<double>(cap);
    int step = std::max(1, static_cast<int>(std::sqrt(ratio)) - 1); // just below the answer, then up
    while (pixelsAt(step) > cap) {
        ++step;
    }
    return step;
}

// The most codes a whole-code bin may hold: see countSamples. Wider, and the bins are laid out as for
// floating point, as for a Fixed range typed as -1e308 to 1e308 on whole numbers.
constexpr double kMostCodesPerBin = 8796093022208.0; // 2^43

// `wholeCodes`: integer bins of whole codes; otherwise the bins are floating point's, in halves.
template <typename T> void countSamples(const cv::Mat& image, Histogram& histogram, bool wholeCodes) {
    const int channels = image.channels();
    const int colours = std::min(channels, 3);
    const auto bins = static_cast<int>(histogram.counts.front().size());
    const double lastBin = static_cast<double>(bins - 1);
    const double origin = histogram.axis.min;
    // Multiplications, not a division per value. Integers: a code plus half lies half a code inside its bin,
    // and the reciprocal's rounding, at most 2^-44 of a bin in 256, stays below that for bins of under 2^43
    // codes (kMostCodesPerBin). Floating point: in halves, as the axis is, so -1e308 to 1e308 stays finite.
    const double perCode = 1.0 / histogram.binWidth;
    const double perHalf = bins / halfSpan(histogram.axis);
    std::vector<std::uint32_t*> curves;
    for (auto& counts : histogram.counts) {
        curves.push_back(counts.data());
    }
    for (int row = 0; row < image.rows; row += histogram.step) {
        const T* in = image.ptr<T>(row);
        for (int col = 0; col < image.cols; col += histogram.step) {
            const T* pixel = in + static_cast<std::ptrdiff_t>(col) * channels;
            for (int channel = 0; channel < colours; ++channel) {
                const auto value = static_cast<double>(pixel[channel]);
                double position = 0.0;
                if constexpr (std::numeric_limits<T>::has_quiet_NaN) {
                    if (!(std::abs(value) <= kLargest)) {
                        continue; // NaN and infinities, counted in the extent
                    }
                }
                if (wholeCodes) {
                    position = (value - origin + 0.5) * perCode;
                } else {
                    position = (value / 2 - origin / 2) * perHalf;
                }
                // Written so that NaN, which no axis should give but which converting to an index would make
                // undefined, lands in the first bin rather than outside the counts.
                const double index = position >= 0.0 ? std::min(position, lastBin) : 0.0;
                ++curves[static_cast<std::size_t>(channel)][static_cast<std::size_t>(index)];
            }
        }
    }
}

void countByDepth(const cv::Mat& image, Histogram& histogram, bool wholeCodes) {
    switch (image.depth()) {
    case CV_8U:
        return countSamples<std::uint8_t>(image, histogram, wholeCodes);
    case CV_8S:
        return countSamples<std::int8_t>(image, histogram, wholeCodes);
    case CV_16U:
        return countSamples<std::uint16_t>(image, histogram, wholeCodes);
    case CV_16S:
        return countSamples<std::int16_t>(image, histogram, wholeCodes);
    case CV_32S:
        return countSamples<std::int32_t>(image, histogram, wholeCodes);
    case CV_32F:
        return countSamples<float>(image, histogram, false);
    default:
        return countSamples<double>(image, histogram, false);
    }
}

} // namespace

Histogram histogramOf(const cv::Mat& image, const ValueExtent& extent,
                      const std::optional<RangeMarkers>& markers, std::size_t maxSamples) {
    checkSupported(image);
    Histogram histogram;
    histogram.depth = image.depth();
    histogram.nonFinite = extent.nonFinite;
    // Set first, so that even a histogram with nothing finite says what it looked at.
    histogram.step = samplingStep(image.rows, image.cols, std::max<std::size_t>(maxSamples, 1));
    histogram.samples = static_cast<std::size_t>((image.rows + histogram.step - 1) / histogram.step) *
                        static_cast<std::size_t>((image.cols + histogram.step - 1) / histogram.step);

    // The data, the range and HDR's top: everything the strip must show.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    const auto include = [&lo, &hi](double value) {
        if (std::isfinite(value)) {
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
    };
    if (extent.colours) {
        include(extent.colours->min);
        include(extent.colours->max);
    }
    if (markers) {
        include(markers->range.min);
        include(markers->range.max);
        if (markers->scaleTop) {
            include(*markers->scaleTop);
        }
    }
    if (lo > hi) {
        return histogram; // nothing finite and no range: the default axis, no curves
    }
    // Equal ends, or two doubles so close that half their difference is 0 (0 and the smallest denormal): one
    // unit around them, or the axis would be a single point.
    if (lo == hi || !(halfSpan({lo, hi}) > 0.0)) {
        lo -= 0.5;
        hi += 0.5;
    }
    // In halves, so that the margin of the widest range stays finite; then clamped to what a double holds.
    const double margin = halfSpan({lo, hi}) * kAxisMargin;
    double low = std::max(2 * (lo / 2 - margin), -kLargest);
    double high = std::min(2 * (hi / 2 + margin), kLargest);

    int bins = kHistogramBins;
    bool wholeCodes = false;
    if (isFloatingPoint(image.depth())) {
        histogram.values = histogram.axis = {low, high};
        histogram.binWidth = halfSpan(histogram.axis) / (kHistogramBins / 2);
    } else {
        // The margin stops at the type's limits, but an end already beyond them, a Fixed range typed past the
        // depth, stays in view and draggable.
        const DisplayRange limits = integerLimits(image.depth());
        low = std::min(lo, std::max(low, limits.min));
        high = std::max(hi, std::min(high, limits.max));
        histogram.values = {std::floor(low), std::ceil(high)};
        // Whole codes per bin, so no bin holds more codes than its neighbours. In doubles: 32S has 2^32
        // codes. A range beyond any such count, typed or sent as -1e308 to 1e308, is not a double's worth of
        // codes: its bins are floating point's.
        const double codes = histogram.values.max - histogram.values.min + 1;
        wholeCodes = std::isfinite(codes) && codes <= kHistogramBins * kMostCodesPerBin;
        if (wholeCodes) {
            const double width = std::ceil(codes / kHistogramBins);
            bins = static_cast<int>(std::ceil(codes / width));
            histogram.binWidth = width;
            histogram.axis = {histogram.values.min, histogram.values.min + bins * width};
        } else {
            histogram.axis = histogram.values;
            histogram.binWidth = halfSpan(histogram.axis) / (kHistogramBins / 2);
        }
    }

    if (extent.colours) {
        histogram.counts.assign(static_cast<std::size_t>(std::min(image.channels(), 3)),
                                std::vector<std::uint32_t>(static_cast<std::size_t>(bins), 0));
        countByDepth(image, histogram, wholeCodes);
    }
    return histogram;
}

double axisFraction(const Histogram& histogram, double value) {
    const double half = halfSpan(histogram.axis);
    return half > 0.0 ? (value / 2 - histogram.axis.min / 2) / half : 0.0;
}

double axisValue(const Histogram& histogram, double fraction) {
    double value = 2 * (histogram.axis.min / 2 + fraction * halfSpan(histogram.axis));
    if (!isFloatingPoint(histogram.depth)) {
        value = std::round(value);
    }
    return std::clamp(value, histogram.values.min, std::max(histogram.values.min, histogram.values.max));
}

DisplayRange draggedRange(const Histogram& histogram, DisplayRange start, RangeHandle handle,
                          double pressFraction, double fraction, double minimumGap) {
    const bool integer = !isFloatingPoint(histogram.depth);
    const double gap = integer ? std::max(1.0, std::ceil(minimumGap)) : std::max(minimumGap, 0.0);
    const DisplayRange& values = histogram.values;
    // In halves, then doubled: infinite only for a drag far off a ±1e308 axis, which the clamps below absorb.
    const double distance = 2 * ((fraction - pressFraction) * halfSpan(histogram.axis));
    const auto snapped = [integer](double value) { return integer ? std::round(value) : value; };
    switch (handle) {
    case RangeHandle::Black: {
        // Never past white less the gap; never below `values`, unless white itself is that close to it.
        const double highest = std::max(values.min, start.max - gap);
        return {std::clamp(snapped(start.min + distance), values.min, highest), start.max};
    }
    case RangeHandle::White: {
        const double lowest = std::min(values.max, start.min + gap);
        return {start.min, std::clamp(snapped(start.max + distance), lowest, values.max)};
    }
    case RangeHandle::Both:
        break;
    }
    // Both ends by the same distance, as far as `values` lets the range go; a range that already spans them
    // cannot move.
    const double fewest = std::min(0.0, values.min - start.min);
    const double most = std::max(0.0, values.max - start.max);
    const double moved = std::clamp(snapped(distance), fewest, most);
    return {start.min + moved, start.max + moved};
}

DisplaySettings settingsForDrag(const DisplaySettings& current, bool hdr, DisplayRange range) {
    DisplaySettings settings = current;
    settings.mode = hdr ? RangeMode::Hdr : RangeMode::Fixed;
    settings.range = range;
    return settings;
}

} // namespace viewer_core
