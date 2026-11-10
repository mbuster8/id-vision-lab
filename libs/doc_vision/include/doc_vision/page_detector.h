#pragma once

#include "doc_vision/onnx_model.h"
#include "doc_vision/page_geometry.h"

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace doc_vision {

// A page found in a frame: its corners (orderedCorners' order) and how sure the model was of the least
// certain corner (the smallest of the four heatmaps' maxima).
struct PageFound {
    Quad quad{};
    double weakestPeak = 0.0;
};

// What finds a page in a frame: DocAligner LC100 in the Document Lab; a scripted one in the tests.
class PageFinder {
public:
    virtual ~PageFinder() = default;
    // Nothing when no page is found; nothing, with `error`, when the model could not run.
    virtual std::optional<PageFound> find(const cv::Mat& bgr8, std::string& error) = 0;
};

// DocsaidLab DocAligner LC100 (Apache-2.0 weights, release 1.1.1): the frame resized to 256 × 256, BGR as it
// is, / 255; four corner heatmaps of 128 × 128 back.
class PageDetector final : public PageFinder {
public:
    static std::unique_ptr<PageDetector> open(const std::filesystem::path& model, std::string& error,
                                              OpenFailure* failure = nullptr,
                                              int intraOpThreads = kDefaultIntraOpThreads);
    std::optional<PageFound> find(const cv::Mat& bgr8, std::string& error) override;
    // The four corner heatmaps (CV_32F, in the model's corner order) for an 8-bit BGR frame; empty, with
    // `error`, when the model could not run.
    std::vector<cv::Mat> heatmaps(const cv::Mat& bgr8, std::string& error);

private:
    explicit PageDetector(std::unique_ptr<OnnxModel> model);
    std::unique_ptr<OnnxModel> m_model;
};

// An 8- or 16-bit image of 1, 3 or 4 channels as 8-bit BGR. A 16-bit image is scaled by its value range when
// it has one (rangeMax > rangeMin), else by 1/257 so 65535 becomes 255; alpha is dropped and grey repeated.
cv::Mat toBgr8(const cv::Mat& image, double rangeMin = 0.0, double rangeMax = 0.0);

// Each heatmap (CV_32F, any size): values under 0.3 set to 0, ×255 to 8 bits, Otsu, the largest external
// contour, its centroid (the bounding box's centre when its area is 0), mapped to the frame as (x + 0.5) · W
// / w − 0.5. Corners in the heatmaps' order; nothing when any heatmap has no blob.
std::optional<std::array<cv::Point2d, 4>> cornersFromHeatmaps(const std::vector<cv::Mat>& heatmaps,
                                                              cv::Size frame);

} // namespace doc_vision
