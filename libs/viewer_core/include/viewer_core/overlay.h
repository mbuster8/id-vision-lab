#pragma once

#include <opencv2/core/types.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace viewer_core {

// The same shapes as visual_debug's overlays (viewer_core does not depend on the transport library, so the
// viewer converts): drawn over an image without touching its pixels. Coordinates are image pixels.

struct Colour {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const Colour&) const = default;
};

inline constexpr Colour kOverlayGreen{0x78, 0xE6, 0x78};

struct Polygon {
    // Standard: a see-through fill, an edge and a label tab, as every overlay. Guide: the Document Lab's page
    // outline, a thick line on a dark halo with no fill and no tab, which reads over any picture.
    enum class Style { Standard, Guide };

    std::vector<cv::Point2d> points{};
    std::string label{};
    Colour colour = kOverlayGreen;
    Style style = Style::Standard;

    bool operator==(const Polygon&) const = default;
};

struct Rectangle {
    cv::Rect2d rect{};
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Rectangle&) const = default;
};

struct Circle {
    cv::Point2d centre{};
    double radius = 0.0;
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Circle&) const = default;
};

struct Point {
    cv::Point2d at{};
    std::string label{};
    Colour colour = kOverlayGreen;

    bool operator==(const Point&) const = default;
};

using Overlay = std::variant<Polygon, Rectangle, Circle, Point>;

} // namespace viewer_core
