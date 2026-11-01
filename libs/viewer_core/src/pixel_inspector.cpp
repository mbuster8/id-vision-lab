#include "viewer_core/pixel_inspector.h"

#include <opencv2/core.hpp>

#include <array>
#include <charconv>
#include <cmath>

namespace viewer_core {
namespace {

template <typename T> double valueAt(const cv::Mat& image, int x, int y, int channel) {
    return static_cast<double>(image.ptr<T>(y)[x * image.channels() + channel]);
}

double sampleAt(const cv::Mat& image, int x, int y, int channel) {
    switch (image.depth()) {
    case CV_8U:
        return valueAt<uchar>(image, x, y, channel);
    case CV_8S:
        return valueAt<schar>(image, x, y, channel);
    case CV_16U:
        return valueAt<ushort>(image, x, y, channel);
    case CV_16S:
        return valueAt<short>(image, x, y, channel);
    case CV_32S:
        return valueAt<int>(image, x, y, channel);
    case CV_32F:
        return valueAt<float>(image, x, y, channel);
    default: // CV_64F
        return valueAt<double>(image, x, y, channel);
    }
}

} // namespace

std::string_view channelLabel(int channels, int channel) {
    static constexpr std::array<std::string_view, 1> kGrey{"I"};
    static constexpr std::array<std::string_view, 2> kTwo{"C0", "C1"};
    static constexpr std::array<std::string_view, 4> kColour{"B", "G", "R", "A"};
    const auto index = static_cast<std::size_t>(channel);
    switch (channels) {
    case 1:
        return kGrey.at(index);
    case 2:
        return kTwo.at(index);
    default:
        return kColour.at(index);
    }
}

// std::to_chars never consults the locale. General format matches printf's %g.
std::string formatSignificant(double value, int digits) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general, digits);
    return std::string(buffer.data(), result.ptr);
}

std::string formatValue(double value, int depth) {
    if (depth == CV_32F || depth == CV_64F || !std::isfinite(value)) {
        return formatSignificant(value, 6);
    }
    return std::to_string(static_cast<long long>(value)); // every integer depth fits a double exactly
}

std::optional<std::string> inspectPixel(const cv::Mat& image, int x, int y) {
    if (image.empty() || image.dims != 2 || x < 0 || y < 0 || x >= image.cols || y >= image.rows ||
        image.channels() > 4) {
        return std::nullopt;
    }
    std::string text = "X=" + std::to_string(x) + " Y=" + std::to_string(y) + " ·";
    for (int channel = 0; channel < image.channels(); ++channel) {
        text += ' ';
        text += channelLabel(image.channels(), channel);
        text += '=';
        // OpenCV 5's further depths (16F and others) are never received, but are not misread either.
        text += image.depth() <= CV_64F ? formatValue(sampleAt(image, x, y, channel), image.depth()) : "?";
    }
    return text;
}

} // namespace viewer_core
