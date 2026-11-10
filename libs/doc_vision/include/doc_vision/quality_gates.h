#pragma once

#include "doc_vision/page_geometry.h"

#include <opencv2/core/mat.hpp>

#include <array>
#include <cstddef>
#include <optional>

namespace doc_vision {

// The six gates, in the chips' order: "In view", "Size", "Flat", "Sharp", "No glare", "Light".
enum class Gate { InView, Size, Flat, Sharp, Glare, Light };
inline constexpr std::size_t kGateCount = 6;

// What the gates judge, measured on a frame and its page's quad.
struct Measures {
    double edgeMargin = 0;  // edgeMarginShare
    double longerSide = 0;  // longerSideShare
    double extent = 0;      // extentShare
    double fill = 0;        // fillShare (shown, not judged)
    double tiltDegrees = 0; // tiltDegrees, with a 70° camera assumed
    double sharpness = 0;   // variance of the Laplacian (64-bit) of the 625 × 440 grey page
    double glareShare = 0;  // share of the page in blobs, of at least 0.1 % of it each, at V ≥ 250 and S ≤ 40
    double medianLuma = 0;  // of the grey page, 0–255
    double clippedShare = 0; // share of the grey page at 250 or more
};

// The measures of an 8-bit BGR frame and its page's quad. `gatePage`, when given, receives the 625 × 440 page
// the gates looked at.
Measures measure(const cv::Mat& bgr8, const Quad& quad, cv::Mat* gatePage = nullptr);

// A gate's two edges: it fails beyond `failsAt` and passes again only beyond `passesAgainAt`, so a measure
// that hovers at the edge does not flip the verdict from frame to frame.
struct Band {
    double failsAt;
    double passesAgainAt;
};

struct Thresholds {
    Band edgeMargin{0.015, 0.020}; // fails under
    Band longerSide{0.40, 0.43};   // fails under: too far
    Band extent{0.92, 0.89};       // fails over: too close
    Band tilt{25.0, 22.0};         // fails over
    Band sharpness{200.0, 220.0};  // fails under
    Band glare{0.010, 0.008};      // fails over
    Band dark{70.0, 75.0};         // median luma; fails under
    Band bright{215.0, 210.0};     // median luma; fails over
    double clippedBright = 0.25;   // a share at 250 or more over this is too bright too
    double fitsWithin =
        0.70; // a cut page whose longer side is under this would fit: "Move the page to the middle"
};

struct Verdicts {
    std::array<bool, kGateCount> passed{}; // Sharp and Glare count as passed when not judged
    bool tooFar = false;
    bool tooClose = false;
    bool tooDark = false;
    bool tooBright = false;
    bool cutButWouldFit = false;

    bool operator==(const Verdicts&) const = default;
    bool allPassed() const;
};

// Judges measures, frame after frame, with each band's hysteresis. With no earlier verdict a measure is
// judged by its failsAt alone. Sharp is not judged while Light fails (a dim page's variance of the Laplacian
// is low however sharp it is), nor Glare while the page is too bright: each then counts as passed and keeps
// its band's state.
class GateJudge {
public:
    explicit GateJudge(Thresholds thresholds = {});
    Verdicts judge(const Measures& measures);
    void reset();
    const Thresholds& thresholds() const { return m_thresholds; }

private:
    struct State {
        std::optional<bool> failing;
    };
    bool lowFails(State& state, const Band& band, double value);  // failing when too low
    bool highFails(State& state, const Band& band, double value); // failing when too high

    Thresholds m_thresholds;
    State m_edgeMargin, m_longerSide, m_extent, m_tilt, m_sharpness, m_glare, m_dark, m_bright;
};

} // namespace doc_vision
