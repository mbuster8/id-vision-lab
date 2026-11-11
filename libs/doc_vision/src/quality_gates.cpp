#include "doc_vision/quality_gates.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <utility>

namespace doc_vision {

namespace {

constexpr int kGlareValue = 250;     // V at or above
constexpr int kGlareSaturation = 40; // S at or below
constexpr double kGlareBlob = 0.001; // a blob counts from this share of the page
constexpr int kClippedLuma = 250;

constexpr std::size_t index(Gate gate) { return static_cast<std::size_t>(gate); }

double medianOf(const cv::Mat& grey) {
    int histogram[256] = {};
    for (int y = 0; y < grey.rows; ++y) {
        const uchar* row = grey.ptr<uchar>(y);
        for (int x = 0; x < grey.cols; ++x) {
            ++histogram[row[x]];
        }
    }
    const std::size_t half = (grey.total() + 1) / 2;
    std::size_t seen = 0;
    for (int value = 0; value < 256; ++value) {
        seen += static_cast<std::size_t>(histogram[value]);
        if (seen >= half) {
            return value;
        }
    }
    return 255;
}

double glareShareOf(const cv::Mat& page) {
    cv::Mat hsv;
    cv::cvtColor(page, hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(0, 0, kGlareValue), cv::Scalar(180, kGlareSaturation, 255), mask);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
    const double smallest = kGlareBlob * static_cast<double>(page.total());
    double glare = 0;
    for (int label = 1; label < count; ++label) {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area >= smallest) {
            glare += area;
        }
    }
    return glare / static_cast<double>(page.total());
}

} // namespace

bool Verdicts::allPassed() const {
    return std::all_of(passed.begin(), passed.end(), [](bool p) { return p; });
}

Measures measure(const cv::Mat& bgr8, const Quad& quad, cv::Mat* gatePage) {
    Measures m;
    const cv::Size frame = bgr8.size();
    m.edgeMargin = edgeMarginShare(quad, frame);
    m.longerSide = longerSideShare(quad, frame);
    m.extent = extentShare(quad, frame);
    m.fill = fillShare(quad, frame);
    m.tiltDegrees = tiltDegrees(quad, frame);

    const cv::Mat page = flatten(bgr8, quad, kGatePageSize);
    cv::Mat grey;
    cv::cvtColor(page, grey, cv::COLOR_BGR2GRAY);
    cv::Mat laplacian;
    cv::Laplacian(grey, laplacian, CV_64F);
    cv::Scalar mean, deviation;
    cv::meanStdDev(laplacian, mean, deviation);
    m.sharpness = deviation[0] * deviation[0];
    m.glareShare = glareShareOf(page);
    m.medianLuma = medianOf(grey);
    m.clippedShare =
        static_cast<double>(cv::countNonZero(grey >= kClippedLuma)) / static_cast<double>(grey.total());
    if (gatePage != nullptr) {
        *gatePage = page;
    }
    return m;
}

GateJudge::GateJudge(Thresholds thresholds) : m_thresholds(thresholds) {}

bool GateJudge::lowFails(State& state, const Band& band, double value) {
    const bool failing = state.failing.value_or(false) ? value < band.passesAgainAt : value < band.failsAt;
    state.failing = failing;
    return failing;
}

bool GateJudge::highFails(State& state, const Band& band, double value) {
    const bool failing = state.failing.value_or(false) ? value > band.passesAgainAt : value > band.failsAt;
    state.failing = failing;
    return failing;
}

Verdicts GateJudge::judge(const Measures& measures) {
    const Thresholds& t = m_thresholds;
    Verdicts v;
    const bool cut = lowFails(m_edgeMargin, t.edgeMargin, measures.edgeMargin);
    v.passed[index(Gate::InView)] = !cut;
    v.cutButWouldFit = cut && measures.longerSide < t.fitsWithin;

    v.tooFar = lowFails(m_longerSide, t.longerSide, measures.longerSide);
    v.tooClose = highFails(m_extent, t.extent, measures.extent);
    v.passed[index(Gate::Size)] = !v.tooFar && !v.tooClose;

    v.passed[index(Gate::Flat)] = !highFails(m_tilt, t.tilt, measures.tiltDegrees);

    v.tooDark = lowFails(m_dark, t.dark, measures.medianLuma);
    v.tooBright =
        highFails(m_bright, t.bright, measures.medianLuma) || measures.clippedShare > t.clippedBright;
    v.passed[index(Gate::Light)] = !v.tooDark && !v.tooBright;

    // Judged only in light they can be judged in; otherwise passed, their bands left as they were.
    v.passed[index(Gate::Sharp)] =
        !v.passed[index(Gate::Light)] || !lowFails(m_sharpness, t.sharpness, measures.sharpness);
    v.passed[index(Gate::Glare)] = v.tooBright || !highFails(m_glare, t.glare, measures.glareShare);
    return v;
}

void GateJudge::reset() {
    for (State* state :
         {&m_edgeMargin, &m_longerSide, &m_extent, &m_tilt, &m_sharpness, &m_glare, &m_dark, &m_bright}) {
        state->failing.reset();
    }
}

double regionSharpness(const cv::Mat& bgr8, cv::Rect region) {
    if (bgr8.empty()) {
        return 0.0;
    }
    const cv::Rect inside = region & cv::Rect(0, 0, bgr8.cols, bgr8.rows);
    if (inside.empty()) {
        return 0.0;
    }
    cv::Mat grey;
    cv::cvtColor(bgr8(inside), grey, cv::COLOR_BGR2GRAY);
    cv::Mat laplacian;
    cv::Laplacian(grey, laplacian, CV_64F);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(laplacian, mean, deviation);
    return deviation[0] * deviation[0];
}

} // namespace doc_vision
