#pragma once

#include "viewer_core/display_settings.h"
#include "viewer_core/image_tags.h"

#include <opencv2/core/mat.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace viewer_core {

// The whole frame's finite minimum and maximum, and how many colour values are not finite (floating point
// only: integers have none). Taken over the whole frame, not a sample, so a lone hot pixel still counts.
struct ValueExtent {
    std::optional<DisplayRange> all;     // every channel, alpha included: what autoRange takes
    std::optional<DisplayRange> colours; // the colour channels only (alpha left out): what the axis spans
    std::size_t nonFinite = 0;           // colour channels only, as the histogram's bins
    bool operator==(const ValueExtent&) const = default;
};

// Throws std::invalid_argument for an image toDisplayImage cannot show.
ValueExtent valueExtent(const cv::Mat& image);

// What maps to black and to white in the picture, and for HDR the top of the scale.
struct RangeMarkers {
    DisplayRange range;
    std::optional<double> scaleTop; // HDR only: hdrScaleTop
    bool hdr = false;               // a drag moves HDR's black and white instead of making Fixed
    bool operator==(const RangeMarkers&) const = default;
};

// The markers for an image of `depth` and `channels` shown with these settings and tags, `extent` being that
// image's. Nothing for Auto without a hint on a non-8U image with no finite value: there is no range to draw.
std::optional<RangeMarkers> rangeMarkers(int depth, int channels, const DisplaySettings& settings,
                                         const ImageTags& tags, const ValueExtent& extent);

inline constexpr int kHistogramBins = 256;
inline constexpr double kAxisMargin = 1.0 / 16; // of the data-and-range span, on each side
// Pixels counted at most per image; above it, an even grid of them: a 4K frame every 8th pixel across and
// down, about 500 samples a bin. Measured on an M4 Pro in a release build (HistogramMeasurement, the strip's
// PaintCost), a 4K 16UC3 frame's extent and counts took 0.52 ms at this cap, 0.72 ms at 2^18 and 1.28 ms at
// 2^19; the strip's paint, labels included, 0.37 ms at 1x and 0.76 ms on a Retina backing store. The paint is
// what passes the 1 ms aimed at, and no cap changes it: this is the smallest cap measured.
inline constexpr std::size_t kHistogramSamples = std::size_t{1} << 17;

// The original values of an image's colour channels, counted into bins across an axis that spans both the
// values and the range markers, so that what the range cuts off is in view and an end can be dragged past the
// data.
struct Histogram {
    DisplayRange axis{0, 1};   // what the strip draws: the bins fill it exactly
    DisplayRange values{0, 1}; // what a drag can reach: data, markers and margin; whole codes for integers
    int depth = CV_8U;         // integer depths have whole-code bins and snap values to codes
    double binWidth = 1.0 / kHistogramBins;         // in values
    std::vector<std::vector<std::uint32_t>> counts; // per colour channel, alpha left out, in OpenCV's order;
                                                    // all the same length; empty when nothing is finite
    int step = 1;                                   // every step-th pixel across and down was counted
    std::size_t samples = 0;   // pixels counted; 0 only for a histogram of nothing, as Histogram{} is
    std::size_t nonFinite = 0; // the whole frame's, from the extent
};

// `extent` and `markers` are the image's (valueExtent, rangeMarkers). Images of more than `maxSamples` pixels
// are counted on an even grid from the top-left pixel, every step-th pixel across and down; a cap of 0 counts
// as 1.
Histogram histogramOf(const cv::Mat& image, const ValueExtent& extent,
                      const std::optional<RangeMarkers>& markers, std::size_t maxSamples = kHistogramSamples);

// Where a value lies along the axis: 0 at axis.min, 1 at axis.max; not clamped. Computed in halves, so that
// an axis as wide as -1e308 to 1e308 stays finite.
double axisFraction(const Histogram& histogram, double value);
// The value at a fraction of the axis, clamped to `values`, and rounded to a whole code for integer depths.
double axisValue(const Histogram& histogram, double fraction);

enum class RangeHandle { Black, White, Both };

// `start` after a drag from pressFraction to fraction of the axis. Black or White: that end moves by the
// drag's distance, snapped and clamped as axisValue, and keeps at least the gap from the other end. Both: the
// distance (whole codes for integers) is limited so that both ends stay within `values`, keeping the width.
// The gap is minimumGap, and at least one code for integer depths. The ends never cross.
DisplayRange draggedRange(const Histogram& histogram, DisplayRange start, RangeHandle handle,
                          double pressFraction, double fraction, double minimumGap);

// What a drag asks for: HDR with `range` as its black and white when the drag began on HDR's markers, Fixed
// otherwise; the colour map kept.
DisplaySettings settingsForDrag(const DisplaySettings& current, bool hdr, DisplayRange range);

} // namespace viewer_core
