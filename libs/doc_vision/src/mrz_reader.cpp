#include "doc_vision/mrz_reader.h"

#include <opencv2/imgproc.hpp>
// OpenCV 5 moved minAreaRect, boxPoints, contourArea and getPerspectiveTransform into its geometry module.
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <string_view>
#include <utility>

namespace doc_vision {

namespace {

constexpr int kDetectionSide = 256;
constexpr int kRecognitionWidth = 640;
constexpr int kRecognitionHeight = 64;
constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789<";

using Clock = std::chrono::steady_clock;

// HWC 8-bit BGR resized to width × height, as CHW floats / 255, BGR as it is: the models were measured so.
std::vector<float> tensorOf(const cv::Mat& bgr8, int width, int height) {
    cv::Mat resized;
    cv::resize(bgr8, resized, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
    std::vector<float> tensor(static_cast<std::size_t>(3) * static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                tensor[(static_cast<std::size_t>(c) * static_cast<std::size_t>(height) +
                        static_cast<std::size_t>(y)) *
                           static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)] = row[x][c] / 255.0f;
            }
        }
    }
    return tensor;
}

// The band warped flat to its own size, long side across, as the spike's MrzScanner::warp.
cv::Mat warpBand(const cv::Mat& page, const Quad& band) {
    const std::vector<cv::Point2f> from(band.begin(), band.end());
    const cv::RotatedRect box = cv::minAreaRect(from);
    int width = static_cast<int>(box.size.width);
    int height = static_cast<int>(box.size.height);
    if (width < height) {
        std::swap(width, height);
    }
    width = std::max(width, 1);
    height = std::max(height, 1);
    const std::vector<cv::Point2f> to{{0.0f, 0.0f},
                                      {static_cast<float>(width), 0.0f},
                                      {static_cast<float>(width), static_cast<float>(height)},
                                      {0.0f, static_cast<float>(height)}};
    cv::Mat flat;
    cv::warpPerspective(page, flat, cv::getPerspectiveTransform(from, to), cv::Size(width, height));
    return flat;
}

bool inTopThird(const Quad& band, int rows) {
    const double centre = (band[0].y + band[1].y + band[2].y + band[3].y) / 4.0;
    return centre < rows / 3.0;
}

} // namespace

std::vector<std::string> decodeMrzTokens(const Tensor& output) {
    if (output.shape.size() != 3 || output.shape[1] <= 0 || output.shape[2] <= 0) {
        return {};
    }
    const auto steps = static_cast<std::size_t>(output.shape[1]);
    const auto classes = static_cast<std::size_t>(output.shape[2]);
    if (output.data.size() < steps * classes) {
        return {};
    }
    std::vector<std::string> lines(1);
    for (std::size_t step = 0; step < steps; ++step) {
        const auto first = output.data.begin() + static_cast<std::ptrdiff_t>(step * classes);
        const auto best = static_cast<std::size_t>(
            std::max_element(first, first + static_cast<std::ptrdiff_t>(classes)) - first);
        if (best == 1) { // EOS
            break;
        }
        if (best == 0) { // PAD
            continue;
        }
        if (best == 2) { // SEP: the next line
            lines.emplace_back();
        } else if (best - 3 < kAlphabet.size()) {
            lines.back() += kAlphabet[best - 3];
        }
    }
    return lines;
}

MrzScanner::MrzScanner(std::unique_ptr<OnnxModel> detection, std::unique_ptr<OnnxModel> recognition)
    : m_detection(std::move(detection)), m_recognition(std::move(recognition)) {}

std::unique_ptr<MrzScanner> MrzScanner::open(const std::filesystem::path& detection,
                                             const std::filesystem::path& recognition, std::string& error,
                                             OpenFailure* failure, int intraOpThreads) {
    std::unique_ptr<OnnxModel> detector = OnnxModel::open(detection, error, intraOpThreads, failure);
    if (!detector) {
        return nullptr;
    }
    std::unique_ptr<OnnxModel> recogniser = OnnxModel::open(recognition, error, intraOpThreads, failure);
    if (!recogniser) {
        return nullptr;
    }
    return std::unique_ptr<MrzScanner>(new MrzScanner(std::move(detector), std::move(recogniser)));
}

void MrzScanner::cancel() {
    m_detection->cancel();
    m_recognition->cancel();
}

MrzReading MrzScanner::read(const cv::Mat& page, std::string& error) {
    MrzReading reading;
    if (page.empty() || page.type() != CV_8UC3) {
        error = "the page is not an 8-bit BGR picture";
        return reading;
    }
    try {
        const Clock::time_point started = Clock::now();
        // Padded to a square with black, as DocsaidLab's own code does.
        const int top = page.rows < page.cols ? (page.cols - page.rows) / 2 : 0;
        const int left = page.cols < page.rows ? (page.rows - page.cols) / 2 : 0;
        cv::Mat padded;
        cv::copyMakeBorder(page, padded, top, top, left, left, cv::BORDER_CONSTANT, cv::Scalar::all(0));
        const std::vector<Tensor> heat = m_detection->run(tensorOf(padded, kDetectionSide, kDetectionSide),
                                                          {1, 3, kDetectionSide, kDetectionSide}, error);
        if (heat.empty() || heat.front().shape.size() < 2) {
            if (error.empty()) {
                error = "the MRZ detector gave no heatmap";
            }
            return reading;
        }
        const Tensor& map = heat.front();
        const int mapHeight = static_cast<int>(map.shape[map.shape.size() - 2]);
        const int mapWidth = static_cast<int>(map.shape.back());
        if (mapHeight <= 0 || mapWidth <= 0 ||
            map.data.size() < static_cast<std::size_t>(mapHeight) * static_cast<std::size_t>(mapWidth)) {
            error = "the MRZ detector's heatmap has an unexpected shape";
            return reading;
        }
        const cv::Mat heatmap(mapHeight, mapWidth, CV_32F, const_cast<float*>(map.data.data()));
        double peak = 0.0;
        cv::minMaxLoc(heatmap, nullptr, &peak);
        reading.peak = peak;
        cv::Mat eightBit;
        heatmap.convertTo(eightBit, CV_8U, 255.0);
        cv::Mat full;
        cv::resize(eightBit, full, padded.size(), 0, 0, cv::INTER_LINEAR);
        cv::Mat binary;
        cv::threshold(full, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        const auto largest =
            std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });
        reading.detectTime = Clock::now() - started;
        if (largest == contours.end() || largest->size() < 3) {
            return reading;
        }
        const cv::RotatedRect box = cv::minAreaRect(*largest);
        const double longSide = std::max(box.size.width, box.size.height);
        const double shortSide = std::min(box.size.width, box.size.height);
        reading.widthShare = longSide / page.cols;
        reading.aspect = longSide / std::max(shortSide, 1.0);
        if (reading.peak < kBandPeak || reading.widthShare < kBandWidthShare ||
            reading.aspect < kBandAspect) {
            return reading;
        }
        cv::Point2f corners[4];
        box.points(corners);
        std::array<cv::Point2d, 4> points{};
        for (std::size_t i = 0; i < 4; ++i) {
            points[i] = cv::Point2d(corners[i].x - left, corners[i].y - top);
        }
        const Quad band = orderedCorners(points);

        const Clock::time_point recognising = Clock::now();
        const std::vector<Tensor> out =
            m_recognition->run(tensorOf(warpBand(page, band), kRecognitionWidth, kRecognitionHeight),
                               {1, 3, kRecognitionHeight, kRecognitionWidth}, error);
        reading.recogniseTime = Clock::now() - recognising;
        if (out.empty()) {
            if (error.empty()) {
                error = "the MRZ recogniser gave no output";
            }
            return reading;
        }
        reading.band = band;
        reading.lines = decodeMrzTokens(out.front());
        return reading;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "the MRZ reader failed";
    }
    return MrzReading{};
}

MrzReading readUpright(MrzReader& reader, const cv::Mat& page, bool turnFirst, bool& turned,
                       std::string& error) {
    const auto turnedPage = [&page] {
        cv::Mat out;
        cv::rotate(page, out, cv::ROTATE_180);
        return out;
    };
    const cv::Mat first = turnFirst ? turnedPage() : page;
    turned = turnFirst;
    MrzReading reading = reader.read(first, error);
    if (!error.empty() || !reading.band || !inTopThird(*reading.band, first.rows)) {
        return reading;
    }
    const cv::Mat second = turnFirst ? page : turnedPage();
    std::string secondError;
    MrzReading other = reader.read(second, secondError);
    if (secondError.empty() && other.band && !inTopThird(*other.band, second.rows)) {
        turned = !turnFirst;
        return other;
    }
    return reading;
}

} // namespace doc_vision
