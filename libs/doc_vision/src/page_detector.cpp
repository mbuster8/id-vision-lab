#include "doc_vision/page_detector.h"

#include <opencv2/imgproc.hpp>
// OpenCV 5 moved contourArea and moments into its geometry module.
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif

#include <algorithm>
#include <utility>

namespace doc_vision {

namespace {

constexpr int kInputSide = 256;
constexpr double kHeatThreshold = 0.3;

// HWC 8-bit BGR, resized to the model's input, as CHW floats / 255. BGR as it is: the model was measured so.
std::vector<float> inputTensor(const cv::Mat& bgr8) {
    cv::Mat resized;
    cv::resize(bgr8, resized, cv::Size(kInputSide, kInputSide), 0, 0, cv::INTER_LINEAR);
    std::vector<float> tensor(static_cast<std::size_t>(3) * kInputSide * kInputSide);
    for (int y = 0; y < kInputSide; ++y) {
        const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
        for (int x = 0; x < kInputSide; ++x) {
            for (int c = 0; c < 3; ++c) {
                tensor[(static_cast<std::size_t>(c) * kInputSide + y) * kInputSide + x] = row[x][c] / 255.0f;
            }
        }
    }
    return tensor;
}

// The largest blob's centroid in heatmap cells, or nothing.
std::optional<cv::Point2d> blobCentre(const cv::Mat& heat) {
    cv::Mat kept;
    cv::threshold(heat, kept, kHeatThreshold, 0, cv::THRESH_TOZERO);
    cv::Mat eightBit;
    kept.convertTo(eightBit, CV_8U, 255.0);
    cv::Mat binary;
    cv::threshold(eightBit, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty() || cv::countNonZero(kept) == 0) {
        return std::nullopt;
    }
    const auto largest = std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
        return cv::contourArea(a) < cv::contourArea(b);
    });
    const cv::Moments m = cv::moments(*largest);
    if (m.m00 <= 0) {
        const cv::Rect box = cv::boundingRect(*largest);
        return cv::Point2d(box.x + box.width / 2.0, box.y + box.height / 2.0);
    }
    return cv::Point2d(m.m10 / m.m00, m.m01 / m.m00);
}

} // namespace

PageDetector::PageDetector(std::unique_ptr<OnnxModel> model) : m_model(std::move(model)) {}

std::unique_ptr<PageDetector> PageDetector::open(const std::filesystem::path& model, std::string& error,
                                                 OpenFailure* failure, int intraOpThreads) {
    std::unique_ptr<OnnxModel> opened = OnnxModel::open(model, error, intraOpThreads, failure);
    if (!opened) {
        return nullptr;
    }
    return std::unique_ptr<PageDetector>(new PageDetector(std::move(opened)));
}

std::vector<cv::Mat> PageDetector::heatmaps(const cv::Mat& bgr8, std::string& error) {
    if (bgr8.empty() || bgr8.type() != CV_8UC3) {
        error = "the page detector takes an 8-bit BGR frame";
        return {};
    }
    const std::vector<Tensor> outputs =
        m_model->run(inputTensor(bgr8), {1, 3, kInputSide, kInputSide}, error);
    if (outputs.empty()) {
        return {};
    }
    const Tensor* heat = &outputs.front();
    for (const Tensor& output : outputs) {
        if (output.name == "heatmap") {
            heat = &output;
        }
    }
    if (heat->shape.size() != 4 || heat->shape[1] < 4) {
        error = "the page model gave no four heatmaps";
        return {};
    }
    const int h = static_cast<int>(heat->shape[2]);
    const int w = static_cast<int>(heat->shape[3]);
    std::vector<cv::Mat> maps;
    for (int k = 0; k < 4; ++k) {
        // A copy: the tensor's storage goes with `outputs`.
        maps.push_back(
            cv::Mat(h, w, CV_32F, const_cast<float*>(heat->data.data()) + static_cast<std::size_t>(k) * h * w)
                .clone());
    }
    return maps;
}

std::optional<PageFound> PageDetector::find(const cv::Mat& bgr8, std::string& error) {
    const std::vector<cv::Mat> maps = heatmaps(bgr8, error);
    if (maps.size() != 4) {
        return std::nullopt;
    }
    const auto corners = cornersFromHeatmaps(maps, bgr8.size());
    if (!corners) {
        return std::nullopt;
    }
    double weakest = 1.0;
    for (const cv::Mat& map : maps) {
        double peak = 0;
        cv::minMaxLoc(map, nullptr, &peak);
        weakest = std::min(weakest, peak);
    }
    return PageFound{orderedCorners(*corners), weakest};
}

cv::Mat toBgr8(const cv::Mat& image, double rangeMin, double rangeMax) {
    cv::Mat eightBit;
    if (image.depth() == CV_16U) {
        if (rangeMax > rangeMin) {
            const double alpha = 255.0 / (rangeMax - rangeMin);
            image.convertTo(eightBit, CV_8U, alpha, -rangeMin * alpha);
        } else {
            image.convertTo(eightBit, CV_8U, 1.0 / 257.0);
        }
    } else {
        eightBit = image;
    }
    cv::Mat bgr;
    switch (eightBit.channels()) {
    case 1:
        cv::cvtColor(eightBit, bgr, cv::COLOR_GRAY2BGR);
        break;
    case 4:
        cv::cvtColor(eightBit, bgr, cv::COLOR_BGRA2BGR);
        break;
    default:
        bgr = eightBit;
        break;
    }
    return bgr;
}

std::optional<std::array<cv::Point2d, 4>> cornersFromHeatmaps(const std::vector<cv::Mat>& heatmaps,
                                                              cv::Size frame) {
    if (heatmaps.size() != 4) {
        return std::nullopt;
    }
    std::array<cv::Point2d, 4> corners;
    for (std::size_t k = 0; k < 4; ++k) {
        const std::optional<cv::Point2d> centre = blobCentre(heatmaps[k]);
        if (!centre) {
            return std::nullopt;
        }
        // Cell centres sit at half-cell positions in the frame, as cv::resize maps them.
        const double sx = static_cast<double>(frame.width) / heatmaps[k].cols;
        const double sy = static_cast<double>(frame.height) / heatmaps[k].rows;
        corners[k] = {(centre->x + 0.5) * sx - 0.5, (centre->y + 0.5) * sy - 0.5};
    }
    return corners;
}

} // namespace doc_vision
