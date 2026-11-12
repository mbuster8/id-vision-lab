#include "doc_vision/quality_gates.h"

#include "procedural_page.h"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

namespace {

using doc_vision::Gate;
using doc_vision::Measures;

constexpr std::size_t at(Gate gate) { return static_cast<std::size_t>(gate); }

// Measures every gate passes; each test changes only what it is about.
Measures goodMeasures() {
    Measures m;
    m.edgeMargin = 0.05;
    m.longerSide = 0.60;
    m.extent = 0.70;
    m.fill = 0.40;
    m.tiltDegrees = 5;
    m.sharpness = 500;
    m.glareShare = 0;
    m.medianLuma = 128;
    m.clippedShare = 0.01;
    return m;
}

const doc_vision::Quad kWholeGatePage{{{0, 0}, {624, 0}, {624, 439}, {0, 439}}};

TEST(QualityGates, GoodMeasuresPassEveryGate) {
    doc_vision::GateJudge judge;
    const doc_vision::Verdicts verdicts = judge.judge(goodMeasures());
    EXPECT_TRUE(verdicts.allPassed());
    EXPECT_FALSE(verdicts.tooFar || verdicts.tooClose || verdicts.tooDark || verdicts.tooBright ||
                 verdicts.cutButWouldFit);
}

TEST(QualityGates, TheProceduralPagePassesEveryGate) {
    const cv::Mat frame =
        test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, {1280, 720});
    const Measures m = doc_vision::measure(frame, test_support::kPageQuad);
    doc_vision::GateJudge judge;
    const doc_vision::Verdicts verdicts = judge.judge(m);
    EXPECT_TRUE(verdicts.allPassed())
        << "margin " << m.edgeMargin << ", longer side " << m.longerSide << ", extent " << m.extent
        << ", tilt " << m.tiltDegrees << ", sharpness " << m.sharpness << ", glare " << m.glareShare
        << ", median " << m.medianLuma << ", clipped " << m.clippedShare;
}

TEST(QualityGates, GlareCountsOnlyLargeBlobs) {
    cv::Mat frame(doc_vision::kGatePageSize, CV_8UC3, cv::Scalar::all(128));
    cv::rectangle(frame, cv::Rect(100, 100, 30, 30), cv::Scalar::all(255), cv::FILLED);
    EXPECT_NEAR(doc_vision::measure(frame, kWholeGatePage).glareShare, 900.0 / 275000.0, 1e-9);

    cv::Mat small(doc_vision::kGatePageSize, CV_8UC3, cv::Scalar::all(128));
    cv::rectangle(small, cv::Rect(100, 100, 10, 10), cv::Scalar::all(255), cv::FILLED);
    EXPECT_DOUBLE_EQ(doc_vision::measure(small, kWholeGatePage).glareShare, 0.0);
}

TEST(QualityGates, LightMeasures) {
    const cv::Mat dim(doc_vision::kGatePageSize, CV_8UC3, cv::Scalar::all(60));
    const Measures dark = doc_vision::measure(dim, kWholeGatePage);
    EXPECT_DOUBLE_EQ(dark.medianLuma, 60.0);
    EXPECT_DOUBLE_EQ(dark.clippedShare, 0.0);

    const cv::Mat glaring(doc_vision::kGatePageSize, CV_8UC3, cv::Scalar::all(252));
    EXPECT_DOUBLE_EQ(doc_vision::measure(glaring, kWholeGatePage).clippedShare, 1.0);
}

TEST(QualityGates, BlurLowersSharpness) {
    const cv::Mat frame =
        test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, {1280, 720});
    EXPECT_GT(doc_vision::measure(frame, test_support::kPageQuad).sharpness, 400.0);
    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(0, 0), 3.0);
    EXPECT_LT(doc_vision::measure(blurred, test_support::kPageQuad).sharpness, 200.0);
}

// A click-to-focus region's sharpness, at the frame's own resolution.
TEST(QualityGates, RegionSharpnessFallsWithBlur) {
    const cv::Mat frame =
        test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, {1280, 720});
    const cv::Rect region(400, 200, 300, 300);
    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(0, 0), 3.0);
    const double sharp = doc_vision::regionSharpness(frame, region);
    EXPECT_GT(sharp, 3.0 * doc_vision::regionSharpness(blurred, region));
}

// A region past the frame's edge is measured on the part inside it; nothing inside gives 0.
TEST(QualityGates, RegionSharpnessClipsToTheFrame) {
    const cv::Mat frame =
        test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, {1280, 720});
    const double clipped = doc_vision::regionSharpness(frame, cv::Rect(900, 550, 400, 400));
    EXPECT_GT(clipped, 0.0);
    EXPECT_DOUBLE_EQ(clipped, doc_vision::regionSharpness(frame, cv::Rect(900, 550, 380, 170)));
    EXPECT_DOUBLE_EQ(doc_vision::regionSharpness(frame, cv::Rect(2000, 2000, 50, 50)), 0.0);
    EXPECT_DOUBLE_EQ(doc_vision::regionSharpness(frame, cv::Rect(10, 10, 0, 0)), 0.0);
    EXPECT_DOUBLE_EQ(doc_vision::regionSharpness(cv::Mat(), cv::Rect(0, 0, 10, 10)), 0.0);
}

TEST(QualityGates, HysteresisOnTheMargin) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    for (const auto& [margin, passes] :
         {std::pair{0.015, true}, std::pair{0.014, false}, std::pair{0.018, false}, std::pair{0.021, true}}) {
        m.edgeMargin = margin;
        EXPECT_EQ(judge.judge(m).passed[at(Gate::InView)], passes) << margin;
    }
    doc_vision::GateJudge fresh;
    m.edgeMargin = 0.0149;
    EXPECT_FALSE(fresh.judge(m).passed[at(Gate::InView)]);
}

TEST(QualityGates, ResetForgetsTheBands) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.edgeMargin = 0.010;
    judge.judge(m);
    judge.reset();
    m.edgeMargin = 0.016;
    EXPECT_TRUE(judge.judge(m).passed[at(Gate::InView)]);
}

// Sharp fails under 50 and passes again from 55: the C920's pages read their MRZ exactly from 50 up, where
// 200 let none of them through (measured 11 November).
TEST(QualityGates, SharpFailsUnderFiftyAndPassesAgainFromFiftyFive) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.sharpness = 49;
    EXPECT_FALSE(judge.judge(m).passed[at(Gate::Sharp)]);
    m.sharpness = 52;
    EXPECT_FALSE(judge.judge(m).passed[at(Gate::Sharp)]); // inside the band: still failing
    m.sharpness = 56;
    EXPECT_TRUE(judge.judge(m).passed[at(Gate::Sharp)]);

    doc_vision::GateJudge fresh;
    m.sharpness = 51;
    EXPECT_TRUE(fresh.judge(m).passed[at(Gate::Sharp)]);
}

TEST(QualityGates, DarkPageSaysDarkNotBlurred) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.medianLuma = 60;
    m.sharpness = 20;
    const doc_vision::Verdicts verdicts = judge.judge(m);
    EXPECT_FALSE(verdicts.passed[at(Gate::Light)]);
    EXPECT_TRUE(verdicts.tooDark);
    EXPECT_TRUE(verdicts.passed[at(Gate::Sharp)]);
}

TEST(QualityGates, BrightPageSaysBrightNotGlare) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.medianLuma = 230;
    m.glareShare = 0.30;
    const doc_vision::Verdicts verdicts = judge.judge(m);
    EXPECT_FALSE(verdicts.passed[at(Gate::Light)]);
    EXPECT_TRUE(verdicts.tooBright);
    EXPECT_TRUE(verdicts.passed[at(Gate::Glare)]);
}

TEST(QualityGates, ClippedPageIsTooBright) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.clippedShare = 0.30;
    EXPECT_TRUE(judge.judge(m).tooBright);
}

TEST(QualityGates, GlareOnAPageLitWell) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.glareShare = 0.02;
    EXPECT_FALSE(judge.judge(m).passed[at(Gate::Glare)]);
}

TEST(QualityGates, TooCloseByExtent) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.extent = 0.93;
    const doc_vision::Verdicts close = judge.judge(m);
    EXPECT_FALSE(close.passed[at(Gate::Size)]);
    EXPECT_TRUE(close.tooClose);

    doc_vision::GateJudge fresh;
    m.fill = 0.70;
    m.extent = 0.80;
    EXPECT_TRUE(fresh.judge(m).passed[at(Gate::Size)]);
}

TEST(QualityGates, TooFarByTheLongerSide) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.longerSide = 0.35;
    const doc_vision::Verdicts far = judge.judge(m);
    EXPECT_FALSE(far.passed[at(Gate::Size)]);
    EXPECT_TRUE(far.tooFar);
}

TEST(QualityGates, TiltedAndBlurredFail) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.tiltDegrees = 30;
    m.sharpness = 30;
    const doc_vision::Verdicts verdicts = judge.judge(m);
    EXPECT_FALSE(verdicts.passed[at(Gate::Flat)]);
    EXPECT_FALSE(verdicts.passed[at(Gate::Sharp)]);
}

TEST(QualityGates, CutButWouldFit) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.edgeMargin = 0.005;
    m.longerSide = 0.60;
    EXPECT_TRUE(judge.judge(m).cutButWouldFit);

    doc_vision::GateJudge large;
    m.longerSide = 0.75;
    EXPECT_FALSE(large.judge(m).cutButWouldFit);
}

// A3.1's deferred item: a cut page whose size hovers at the 70% cut-off kept flipping between "Move back" and
// "Move the document to the middle". Too large to fit over 0.70, it fits again only under 0.67.
TEST(QualityGates, CutButWouldFitHasHysteresis) {
    doc_vision::GateJudge judge;
    Measures m = goodMeasures();
    m.edgeMargin = 0.005;
    const auto fitsAt = [&](double longerSide) {
        m.longerSide = longerSide;
        return judge.judge(m).cutButWouldFit;
    };
    EXPECT_TRUE(fitsAt(0.69));
    EXPECT_FALSE(fitsAt(0.71));
    EXPECT_FALSE(fitsAt(0.69)); // still too large: within the band
    EXPECT_FALSE(fitsAt(0.68));
    EXPECT_TRUE(fitsAt(0.66));
    EXPECT_TRUE(fitsAt(0.69)); // fits until over 0.70 again
}

} // namespace
