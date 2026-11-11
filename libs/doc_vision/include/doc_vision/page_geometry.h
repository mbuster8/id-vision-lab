#pragma once

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

#include <array>

namespace doc_vision {

// A page's four corners in frame pixels: top-left, top-right, bottom-right, bottom-left, clockwise in the
// image, the first side (TL to TR) one of the two longer ones, so the page always flattens landscape.
using Quad = std::array<cv::Point2d, 4>;

// A TD3 passport's data page (ICAO Doc 9303), in millimetres.
inline constexpr double kTd3WidthMm = 125.0;
inline constexpr double kTd3HeightMm = 88.0;
// The flattened page at 10 px/mm (the "page" a reader reads), and the half-size page the quality gates
// measure.
inline const cv::Size kPageSize{1250, 880};
inline const cv::Size kGatePageSize{625, 440};

// The points in clockwise order in the image, starting so that the first side is one of the two longer sides
// (by the mean length of opposite sides), and of those, the one whose midpoint is higher in the frame
// (smaller y); of two at the same height, the one further left. Which way up the page is cannot be told from
// its corners; A4 turns it when the MRZ is found at the top.
Quad orderedCorners(const std::array<cv::Point2d, 4>& points);

// `next` shifted by 0 or 2 corners, whichever lies closer to `previous` (least sum of corner distances): a
// page held near 90° keeps its start corner from frame to frame instead of flipping. Never 1 or 3, which
// would make the first side a shorter one.
Quad alignTo(const Quad& next, const Quad& previous);

// The quad's page warped flat to `size`: TL, TR, BR, BL onto (0,0), (w−1,0), (w−1,h−1), (0,h−1), so a frame
// whose quad is its own corners comes back unchanged. Bilinear, the frame's edge replicated.
cv::Mat flatten(const cv::Mat& image, const Quad& quad, cv::Size size);

// The quad's area over the frame's.
double fillShare(const Quad& quad, cv::Size frame);
// The quad's longest side over the frame's longer side.
double longerSideShare(const Quad& quad, cv::Size frame);
// The quad's bounding box over the frame, along whichever dimension it fills more.
double extentShare(const Quad& quad, cv::Size frame);
// The corners' least distance to the frame's edge, min(x, y, W−1−x, H−1−y), over the frame's shorter side;
// negative for a corner outside the frame.
double edgeMarginShare(const Quad& quad, cv::Size frame);
// The page's tilt from facing the camera, in degrees: the angle between the page's plane and the image plane,
// from the homography of the TD3 rectangle to the quad, with a camera of `horizontalFovDegrees` assumed (its
// focal length from the frame's width, its centre the frame's). An estimate: the camera's own field of view
// is unknown.
double tiltDegrees(const Quad& quad, cv::Size frame, double horizontalFovDegrees = 70.0);

} // namespace doc_vision
