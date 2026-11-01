#include "viewer_core/hdr_tone_map.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace viewer_core {
namespace {

// ITU-R BT.2100: HLG's inverse OETF and its OOTF for a 1000 cd/m² display.
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;
constexpr double kHlgPeak = 1000.0;
constexpr double kHlgSystemGamma = 1.2;
// ITU-R BT.2100: PQ's EOTF, to cd/m².
constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = 2523.0 / 32.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = 2413.0 / 128.0;
constexpr double kPqC3 = 2392.0 / 128.0;
constexpr double kPqAssumedPeak = 1000.0;
// ITU-R BT.2408: HDR reference white, in cd/m².
constexpr double kReferenceWhite = 203.0;
// BT.2020 luminance weights, for R, G, B.
constexpr std::array<double, 3> kLuma{0.2627, 0.6780, 0.0593};
// ITU-R BT.2087: linear BT.2020 RGB to BT.709 RGB.
constexpr std::array<std::array<double, 3>, 3> kBt2020ToBt709{{
    {1.6605, -0.5876, -0.0728},
    {-0.1246, 1.1329, -0.0083},
    {-0.0182, -0.1006, 1.1187},
}};
// The most a limited-range code carries above nominal white: 12-bit codes 3761 to 4095 over 256 to 3760.
// --hdr frames (10-bit, about 1.0947) stay below it; a hint far below its depth's top, such as 10-bit codes
// in 16 bits, gets this much headroom instead of a white point the codes never reach.
constexpr double kMaxHlgSignal = (4095.0 - 256.0) / (3760.0 - 256.0);
// Light relative to reference white up to this is shown as it is; above it, compressed into the room left.
constexpr double kKnee = 0.5;
constexpr int kSrgbSteps = 16384;

double hlgSceneLight(double signal) {
    return signal <= 0.5 ? signal * signal / 3.0 : (std::exp((signal - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double pqDisplayLight(double signal) {
    const double power = std::pow(signal, 1.0 / kPqM2);
    return 10000.0 * std::pow(std::max(power - kPqC1, 0.0) / (kPqC2 - kPqC3 * power), 1.0 / kPqM1);
}

// Where `value` lies from `min` (0) to `max` (1), in halves so that no range a double can hold overflows: the
// span from -1e308 to 1e308 is not a double, its half is.
double normalised(double value, double min, double halfSpan) { return (value / 2 - min / 2) / halfSpan; }

// What each channel's signal becomes before the channels are combined: HLG's scene light (0 to 12 at most),
// PQ's display light in cd/m². The signal is normalised by its range and clipped: below at black, and above
// at the HLG white point or at PQ's nominal white. NaN is black.
struct Linearise {
    Transfer transfer;
    double black;
    double halfSpan;
    double maxSignal;

    float operator()(double value) const {
        const double share = normalised(value, black, halfSpan);
        const double signal = std::isnan(share) ? 0.0 : std::clamp(share, 0.0, maxSignal);
        return static_cast<float>(transfer == Transfer::Hlg ? hlgSceneLight(signal) : pqDisplayLight(signal));
    }
};

// Light relative to reference white, as the screen's 0 to 1: unchanged up to the knee, then a logarithm that
// leaves the knee at the same slope and reaches 1 at the white point.
class Compression {
public:
    explicit Compression(double whitePoint) {
        if (whitePoint <= 1.0) {
            return; // nothing to compress: clipped at 1
        }
        // The steepness b solves (1 - knee) · b = log(1 + b · (whitePoint - knee)), which has one positive
        // root when the white point is above 1; bisected on a logarithmic scale.
        const double room = 1.0 - kKnee;
        const double above = whitePoint - kKnee;
        double low = 1e-9;
        double high = 1e9;
        for (int step = 0; step < 200; ++step) {
            const double middle = std::sqrt(low * high);
            if (room * middle < std::log1p(middle * above)) {
                low = middle;
            } else {
                high = middle;
            }
        }
        m_steepness = std::sqrt(low * high);
        m_scale = room / std::log1p(m_steepness * above);
    }

    double operator()(double light) const {
        if (light <= kKnee || m_steepness == 0.0) {
            return light;
        }
        return kKnee + m_scale * std::log1p(m_steepness * (light - kKnee));
    }

private:
    double m_steepness = 0.0;
    double m_scale = 0.0;
};

const std::vector<uchar>& srgbTable() {
    static const std::vector<uchar> table = [] {
        std::vector<uchar> values(kSrgbSteps + 1);
        for (int i = 0; i <= kSrgbSteps; ++i) {
            const double linear = static_cast<double>(i) / kSrgbSteps;
            const double encoded =
                linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
            values[static_cast<std::size_t>(i)] = cv::saturate_cast<uchar>(255.0 * encoded);
        }
        return values;
    }();
    return table;
}

// NaN is black: it must never become an index (std::lround gives 0 for it on ARM, but LONG_MIN on x86-64).
uchar srgb(double linear) {
    const double clipped = std::isnan(linear) ? 0.0 : std::clamp(linear, 0.0, 1.0);
    return srgbTable()[static_cast<std::size_t>(std::lround(clipped * kSrgbSteps))];
}

// Each value through `linearise`, as 32-bit floats with the same channels: a table for 8U and 16U, which
// covers every value, and the formula for the other depths.
cv::Mat linearised(const cv::Mat& signal, const Linearise& linearise) {
    cv::Mat result(signal.size(), CV_32FC(signal.channels()));
    const int valuesPerRow = signal.cols * signal.channels();
    if (signal.depth() == CV_8U || signal.depth() == CV_16U) {
        const int count = signal.depth() == CV_8U ? 256 : 65536;
        std::vector<float> table(static_cast<std::size_t>(count));
        for (int value = 0; value < count; ++value) {
            table[static_cast<std::size_t>(value)] = linearise(value);
        }
        cv::parallel_for_(cv::Range(0, signal.rows), [&](const cv::Range& rows) {
            for (int row = rows.start; row < rows.end; ++row) {
                auto* out = result.ptr<float>(row);
                if (signal.depth() == CV_8U) {
                    const auto* in = signal.ptr<uchar>(row);
                    for (int i = 0; i < valuesPerRow; ++i) {
                        out[i] = table[in[i]];
                    }
                } else {
                    const auto* in = signal.ptr<std::uint16_t>(row);
                    for (int i = 0; i < valuesPerRow; ++i) {
                        out[i] = table[in[i]];
                    }
                }
            }
        });
        return result;
    }
    cv::Mat values;
    signal.convertTo(values, CV_64F);
    cv::parallel_for_(cv::Range(0, signal.rows), [&](const cv::Range& rows) {
        for (int row = rows.start; row < rows.end; ++row) {
            const auto* in = values.ptr<double>(row);
            auto* out = result.ptr<float>(row);
            for (int i = 0; i < valuesPerRow; ++i) {
                out[i] = linearise(in[i]);
            }
        }
    });
    return result;
}

double integerMaximum(int depth) {
    switch (depth) {
    case CV_8U:
        return std::numeric_limits<std::uint8_t>::max();
    case CV_8S:
        return std::numeric_limits<std::int8_t>::max();
    case CV_16U:
        return std::numeric_limits<std::uint16_t>::max();
    case CV_16S:
        return std::numeric_limits<std::int16_t>::max();
    default:
        return std::numeric_limits<std::int32_t>::max();
    }
}

double integerMinimum(int depth) {
    switch (depth) {
    case CV_8S:
        return std::numeric_limits<std::int8_t>::min();
    case CV_16S:
        return std::numeric_limits<std::int16_t>::min();
    case CV_32S:
        return std::numeric_limits<std::int32_t>::min();
    default:
        return 0.0;
    }
}

bool isFloatingPoint(int depth) { return depth == CV_32F || depth == CV_64F; }

} // namespace

DisplayRange hdrSignalRange(int depth, const ImageTags& tags) {
    if (tags.hasValueRange()) {
        return tags.valueRange;
    }
    if (isFloatingPoint(depth)) {
        return {0.0, 1.0};
    }
    return {integerMinimum(depth), integerMaximum(depth)};
}

double hdrScaleTop(int depth, const DisplayRange& signalRange) {
    return isFloatingPoint(depth) ? signalRange.max : integerMaximum(depth);
}

cv::Mat toneMapToSdr(const cv::Mat& signal, Transfer transfer, Primaries primaries,
                     DisplayRange signalRange) {
    if (signal.channels() != 1 && signal.channels() != 3) {
        throw std::invalid_argument("toneMapToSdr: expected 1 or 3 channels");
    }
    if (transfer == Transfer::Sdr) {
        throw std::invalid_argument("toneMapToSdr: an SDR signal needs no tone mapping");
    }
    if (!(std::isfinite(signalRange.min) && std::isfinite(signalRange.max) &&
          signalRange.min < signalRange.max)) {
        throw std::invalid_argument("toneMapToSdr: the signal range must increase");
    }
    const double halfSpan = signalRange.max / 2 - signalRange.min / 2;
    const bool hlg = transfer == Transfer::Hlg;
    const double hlgTop = std::clamp(
        normalised(hdrScaleTop(signal.depth(), signalRange), signalRange.min, halfSpan), 1.0, kMaxHlgSignal);
    const Linearise linearise{transfer, signalRange.min, halfSpan, hlg ? hlgTop : 1.0};
    // The white point, relative to reference white: a grey at the HLG top, or the assumed PQ peak.
    const double whitePoint =
        (hlg ? kHlgPeak * std::pow(hlgSceneLight(hlgTop), kHlgSystemGamma) : kPqAssumedPeak) /
        kReferenceWhite;
    const Compression compress(whitePoint);
    const bool toBt709 = primaries == Primaries::Bt2020;

    const cv::Mat light = linearised(signal, linearise);
    cv::Mat result(signal.size(), CV_8UC(signal.channels()));
    // HLG's light is scene light: the OOTF makes it display light, scaling every channel by its luminance's
    // gain. Then each pixel's luminance is compressed and its channels scaled alike, so its hue holds.
    const auto toDisplay = [hlg](double luminance) {
        return hlg ? (luminance > 0.0 ? kHlgPeak * std::pow(luminance, kHlgSystemGamma - 1.0) : 0.0) : 1.0;
    };
    cv::parallel_for_(cv::Range(0, signal.rows), [&](const cv::Range& rows) {
        for (int row = rows.start; row < rows.end; ++row) {
            const auto* in = light.ptr<float>(row);
            auto* out = result.ptr<uchar>(row);
            if (signal.channels() == 1) {
                for (int x = 0; x < signal.cols; ++x) {
                    const double value = in[x] * toDisplay(in[x]) / kReferenceWhite;
                    out[x] = srgb(std::min(compress(value), 1.0));
                }
                continue;
            }
            for (int x = 0; x < signal.cols; ++x) {
                const float* bgr = in + 3 * x;
                std::array<double, 3> rgb{bgr[2], bgr[1], bgr[0]};
                const double gain =
                    toDisplay(kLuma[0] * rgb[0] + kLuma[1] * rgb[1] + kLuma[2] * rgb[2]) / kReferenceWhite;
                for (double& channel : rgb) {
                    channel *= gain;
                }
                const double luminance = kLuma[0] * rgb[0] + kLuma[1] * rgb[1] + kLuma[2] * rgb[2];
                const double factor = luminance > kKnee ? compress(luminance) / luminance : 1.0;
                for (double& channel : rgb) {
                    channel = std::min(channel * factor, 1.0);
                }
                if (toBt709) {
                    const std::array<double, 3> bt2020 = rgb;
                    for (std::size_t i = 0; i < 3; ++i) {
                        rgb[i] = kBt2020ToBt709[i][0] * bt2020[0] + kBt2020ToBt709[i][1] * bt2020[1] +
                                 kBt2020ToBt709[i][2] * bt2020[2];
                    }
                }
                out[3 * x] = srgb(rgb[2]);
                out[3 * x + 1] = srgb(rgb[1]);
                out[3 * x + 2] = srgb(rgb[0]);
            }
        }
    });
    return result;
}

} // namespace viewer_core
