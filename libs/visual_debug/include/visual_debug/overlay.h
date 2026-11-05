#pragma once

#include <opencv2/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace visual_debug {

// Shapes a sender draws over an image without touching its pixels: the viewer shows each as a see-through
// fill with a solid edge in its colour, and its label on a small tab. Coordinates are image pixels, x to the
// right and y down, (0, 0) the top-left corner of the top-left pixel. docs/wire-format.md has the rules.

struct Colour {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const Colour&) const = default;
};

inline constexpr Colour kOverlayGreen{0x78, 0xE6, 0x78}; // every overlay's colour unless it names another
inline constexpr Colour kOverlayRed{0xE0, 0x50, 0x50};
inline constexpr Colour kOverlayAmber{0xF0, 0xB0, 0x40};

// Closed: the last point joins the first. 3 to kMaxPolygonPoints points.
struct Polygon {
    std::vector<cv::Point2d> points{};
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Polygon&) const = default;
};

struct Rectangle {
    cv::Rect2d rect{}; // top-left corner, width and height above 0
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Rectangle&) const = default;
};

struct Circle {
    cv::Point2d centre{};
    double radius = 0.0; // above 0
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Circle&) const = default;
};

// A dot of a fixed size on screen.
struct Point {
    cv::Point2d at{};
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Point&) const = default;
};

using Overlay = std::variant<Polygon, Rectangle, Circle, Point>;

// What the viewer accepts. The sender leaves out an overlay outside these limits (or past the
// kMaxOverlays-th) rather than send one the viewer would refuse, and cuts a longer label to kMaxLabelBytes.
inline constexpr std::size_t kMaxOverlays = 256;
inline constexpr std::size_t kMaxPolygonPoints = 1024;
inline constexpr std::size_t kMaxLabelBytes = 64;
inline constexpr double kMaxOverlayCoordinate = 1'000'000.0; // for every coordinate, size and radius

// Contours and boxes as OpenCV gives them, as a Polygon's points.
std::vector<cv::Point2d> toPoints(const std::vector<cv::Point>& contour);
std::vector<cv::Point2d> toPoints(const std::vector<cv::Point2f>& contour);
std::vector<cv::Point2d> toPoints(const cv::RotatedRect& box); // its corners, in RotatedRect::points order

} // namespace visual_debug
