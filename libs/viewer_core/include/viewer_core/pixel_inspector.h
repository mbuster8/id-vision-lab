#pragma once

#include <opencv2/core/mat.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace viewer_core {

// The original value of every channel at image coordinate (x, y), for example "X=12 Y=40 · B=12 G=200 R=3",
// or nothing outside the image. Labels follow OpenCV's channel order: I; C0 C1; B G R; B G R A.
std::optional<std::string> inspectPixel(const cv::Mat& image, int x, int y);

// How the readout names a channel of an image with `channels` channels.
std::string_view channelLabel(int channels, int channel);

// How the readout writes a value of an image of this depth: integers whole, floating point to 6 significant
// digits. Never locale-dependent: a German system still prints a decimal point.
std::string formatValue(double value, int depth);

// A number to `digits` significant digits, as printf's %g would write it, never locale-dependent.
std::string formatSignificant(double value, int digits);

} // namespace viewer_core
