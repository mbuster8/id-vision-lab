#pragma once

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

#include <optional>
#include <vector>

namespace lab_core {

// The Canny edges (thresholds 50 and 150) of an 8- or 16-bit greyscale, BGR or BGRA frame (of a 16-bit one,
// its top 8 bits): CV_8UC1, the same size.
cv::Mat edgesOf(const cv::Mat& frame);

// The corners of the largest document-like shape in a frame (any frame edgesOf takes), such as a card or a
// passport page, in the frame's pixels: four corners fitted to an outline's convex hull, covering at least 10
// % of the frame, wholly in view, lighter inside than the frame on average, with no corner sharper than 45°
// or blunter than 135° and no side under a fifth of the longest. Nothing when no shape qualifies. Found on a
// blurred copy at most 640 pixels on its longest side, so a 4K frame costs about what a small one does. A
// demonstration of overlays, not a document detector: it works best with the whole document in view on a
// darker surface; a light page on a light desk, or a page the frame cuts off, is rarely found.
std::optional<std::vector<cv::Point2d>> documentOutline(const cv::Mat& frame);

// How many frames an outline is kept after it is lost: a third of a second at 30 frames a second.
inline constexpr int kOutlineHoldFrames = 10;

// Keeps the last outline found for kOutlineHoldFrames frames after the detector loses it, so a frame or two
// it misses (motion blur, a finger crossing an edge) do not make the outline flicker.
class OutlineHold {
public:
    // The outline to show for a frame in which `found` was found (or nothing was).
    std::optional<std::vector<cv::Point2d>> next(std::optional<std::vector<cv::Point2d>> found);

private:
    std::optional<std::vector<cv::Point2d>> m_last;
    int m_missed = 0;
};

} // namespace lab_core
