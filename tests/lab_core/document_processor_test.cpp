#include "lab_core/document_processor.h"
#include "lab_core/processors.h"

#include "procedural_page.h"

#include <doc_vision/guidance.h>
#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>
#include <doc_vision/quality_gates.h>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using doc_vision::Hint;
using lab_core::DocumentProcessor;
using lab_core::Finding;
using lab_core::Guidance;
using lab_core::ProcessorResult;

// One answer of the scripted finder: a page, nothing, or nothing with an error.
struct Step {
    std::optional<doc_vision::Quad> quad;
    std::string error;
};

Step page(const doc_vision::Quad& quad) { return {.quad = quad, .error = {}}; }
Step nothing() { return {}; }
Step failure(std::string error) { return {.quad = std::nullopt, .error = std::move(error)}; }

// What the scripted finders of one processor share with the test: the answers in order (the last one
// repeats), the frames they were given, and how often the processor asked for a finder.
struct Script {
    std::vector<Step> steps;
    std::size_t calls = 0;
    int opens = 0;
    cv::Mat lastImage;
};

class ScriptedFinder final : public doc_vision::PageFinder {
public:
    explicit ScriptedFinder(std::shared_ptr<Script> script) : m_script(std::move(script)) {}

    std::optional<doc_vision::PageFound> find(const cv::Mat& bgr8, std::string& error) override {
        Script& script = *m_script;
        script.lastImage = bgr8.clone();
        if (script.steps.empty()) {
            return std::nullopt;
        }
        const Step& step = script.steps[std::min(script.calls++, script.steps.size() - 1)];
        error = step.error;
        if (!step.quad) {
            return std::nullopt;
        }
        return doc_vision::PageFound{.quad = *step.quad, .weakestPeak = 1.0};
    }

private:
    std::shared_ptr<Script> m_script;
};

DocumentProcessor::MakeFinder scripted(const std::shared_ptr<Script>& script) {
    return [script](std::string&, doc_vision::OpenFailure&) -> std::unique_ptr<doc_vision::PageFinder> {
        ++script->opens;
        return std::make_unique<ScriptedFinder>(script);
    };
}

const cv::Size kFrameSize{1280, 720};

// The frame TheProceduralPagePassesEveryGate judges: every gate passes.
cv::Mat goodFrame() {
    return test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, kFrameSize);
}

// A page cut by the frame's left edge, too large to fit if moved: "Cut by the edge", not "Move to the
// middle".
const doc_vision::Quad kCutQuad{{{-60, 80}, {1180, 110}, {1160, 690}, {-40, 660}}};

frame_io::Frame frameAt(const cv::Mat& image, int n, visual_debug::ImageDescription tags = {}) {
    return {.image = image,
            .number = static_cast<std::uint64_t>(n),
            .at = std::chrono::milliseconds{33 * n},
            .tags = std::move(tags)};
}

const visual_debug::Polygon& outlineOf(const ProcessorResult& result) {
    static const visual_debug::Polygon none{};
    if (!result.guidance || !result.guidance->outlineOverlay ||
        *result.guidance->outlineOverlay >= result.overlays.size()) {
        ADD_FAILURE() << "no outline";
        return none;
    }
    const auto* polygon =
        std::get_if<visual_debug::Polygon>(&result.overlays[*result.guidance->outlineOverlay]);
    if (!polygon) {
        ADD_FAILURE() << "the outline is not a polygon";
        return none;
    }
    return *polygon;
}

const Finding* findingNamed(const ProcessorResult& result, const std::string& name) {
    for (const Finding& finding : result.findings) {
        if (finding.name == name) {
            return &finding;
        }
    }
    return nullptr;
}

std::vector<std::string> namesOf(const ProcessorResult& result) {
    std::vector<std::string> names;
    for (const Finding& finding : result.findings) {
        names.push_back(finding.name);
    }
    return names;
}

std::vector<Finding::Tone> tonesOf(const ProcessorResult& result) {
    std::vector<Finding::Tone> tones;
    for (const Finding& finding : result.findings) {
        tones.push_back(finding.tone);
    }
    return tones;
}

TEST(DocumentProcessor, NoPageShowsHintOneAtOnce) {
    auto script = std::make_shared<Script>();
    script->steps = {nothing()};
    DocumentProcessor processor(scripted(script));

    const ProcessorResult result = processor.process(frameAt(goodFrame(), 0));

    ASSERT_TRUE(result.guidance);
    EXPECT_EQ(result.guidance->frameHint, Hint::NoPage);
    EXPECT_EQ(result.guidance->shownHint, Hint::NoPage);
    EXPECT_FALSE(result.guidance->chips.visible);
    EXPECT_FALSE(result.guidance->outlineOverlay);
    EXPECT_EQ(result.guidance->unavailable, Guidance::Unavailable::None);
    EXPECT_TRUE(result.overlays.empty());
    EXPECT_TRUE(result.images.empty());
    EXPECT_EQ(namesOf(result), (std::vector<std::string>{"Document", "Gates", "Detector"}));
    EXPECT_EQ(*findingNamed(result, "Document"), (Finding{"Document", "not found", Finding::Tone::Plain}));
    EXPECT_EQ(*findingNamed(result, "Gates"), (Finding{"Gates", "no page yet", Finding::Tone::Plain}));
}

// P1: red at once, green only once Good has held half a second and NoPage its 800 ms.
TEST(DocumentProcessor, GoodPageTurnsGreenAfterHalfASecond) {
    auto script = std::make_shared<Script>();
    script->steps = {nothing(), page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));
    const cv::Mat frame = goodFrame();

    EXPECT_FALSE(processor.process(frameAt(frame, 0)).guidance->outlineOverlay);
    for (int n = 1; n <= 24; ++n) {
        const ProcessorResult result = processor.process(frameAt(frame, n));
        ASSERT_TRUE(result.guidance);
        EXPECT_EQ(result.guidance->frameHint, Hint::Good) << n;
        EXPECT_NE(result.guidance->shownHint, Hint::Good) << n;
        EXPECT_EQ(outlineOf(result).colour, visual_debug::kOverlayRed) << n;
    }
    const ProcessorResult green = processor.process(frameAt(frame, 25)); // 825 ms
    EXPECT_EQ(green.guidance->shownHint, Hint::Good);
    EXPECT_EQ(outlineOf(green).colour, visual_debug::kOverlayGreen);
    EXPECT_EQ(outlineOf(green).label, "Document");
    EXPECT_TRUE(green.guidance->chips.visible);
}

// P6: the engine resets on every seek, loop and restart; the model is opened once.
TEST(DocumentProcessor, ResetKeepsAnOpenedFinder) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));

    processor.process(frameAt(goodFrame(), 0));
    processor.reset();
    processor.process(frameAt(goodFrame(), 1));

    EXPECT_EQ(script->opens, 1);
}

// P23: a model that fails on one frame is a red Detector row on that frame, and the next frame tries again.
TEST(DocumentProcessor, RunTimeErrorIsABadDetectorRow) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad), page(test_support::kPageQuad), failure("boom"),
                     page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));
    const cv::Mat frame = goodFrame();

    processor.process(frameAt(frame, 0));
    processor.process(frameAt(frame, 1));
    const ProcessorResult failed = processor.process(frameAt(frame, 2));
    const ProcessorResult next = processor.process(frameAt(frame, 3));

    ASSERT_NE(findingNamed(failed, "Detector"), nullptr);
    EXPECT_EQ(*findingNamed(failed, "Detector"), (Finding{"Detector", "error: boom", Finding::Tone::Bad}));
    ASSERT_NE(findingNamed(next, "Detector"), nullptr);
    EXPECT_EQ(findingNamed(next, "Detector")->value.rfind("LC100 \xC2\xB7 ", 0), 0U)
        << findingNamed(next, "Detector")->value;
    EXPECT_EQ(findingNamed(next, "Detector")->tone, Finding::Tone::Plain);
}

TEST(DocumentProcessor, FindingsInOrderWithTheirTones) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));
    const cv::Mat frame = goodFrame();

    const ProcessorResult result = processor.process(frameAt(frame, 0));

    EXPECT_EQ(namesOf(result), (std::vector<std::string>{"Document", "Fill", "Tilt", "Sharpness", "Glare",
                                                         "Exposure", "Gates", "Detector"}));
    using Tone = Finding::Tone;
    EXPECT_EQ(tonesOf(result), (std::vector<Tone>{Tone::Plain, Tone::Plain, Tone::Plain, Tone::Plain,
                                                  Tone::Plain, Tone::Plain, Tone::Good, Tone::Plain}));
    const doc_vision::Measures measures = doc_vision::measure(frame, test_support::kPageQuad);
    EXPECT_EQ(findingNamed(result, "Document")->value, "TD3 page");
    EXPECT_EQ(findingNamed(result, "Fill")->value, std::to_string(std::lround(100 * measures.fill)) + " %");
    EXPECT_EQ(findingNamed(result, "Tilt")->value,
              std::to_string(std::lround(measures.tiltDegrees)) + "\xC2\xB0");
    EXPECT_EQ(findingNamed(result, "Sharpness")->value, std::to_string(std::lround(measures.sharpness)));
    EXPECT_EQ(findingNamed(result, "Glare")->value, "0.0 %");
    EXPECT_EQ(findingNamed(result, "Exposure")->value, "ok, 0.0 % clipped");
    EXPECT_EQ(findingNamed(result, "Gates")->value, "all pass");
}

TEST(DocumentProcessor, FailingMeasuresAreBad) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));
    cv::Mat blurred;
    cv::GaussianBlur(goodFrame(), blurred, {0, 0}, 8.0);

    const ProcessorResult result = processor.process(frameAt(blurred, 0));

    const Finding* sharpness = findingNamed(result, "Sharpness");
    ASSERT_NE(sharpness, nullptr);
    EXPECT_EQ(sharpness->tone, Finding::Tone::Bad);
    EXPECT_NE(sharpness->value.find(" (min 200)"), std::string::npos) << sharpness->value;
    EXPECT_EQ(*findingNamed(result, "Gates"), (Finding{"Gates", "5 of 6 pass", Finding::Tone::Bad}));
    EXPECT_EQ(result.guidance->frameHint, Hint::Blurred);
}

// The evaluation tries other thresholds: they are the processor's, not GateJudge's defaults.
TEST(DocumentProcessor, JudgesByTheGivenThresholds) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    doc_vision::Thresholds strict;
    strict.sharpness = {1e9, 1e9};
    DocumentProcessor processor(scripted(script), strict);

    const ProcessorResult result = processor.process(frameAt(goodFrame(), 0));

    EXPECT_EQ(result.guidance->frameHint, Hint::Blurred);
}

// A failing measure is red with the limit it must reach, as Sharpness is: Tilt too.
TEST(DocumentProcessor, TiltedPageIsBadWithItsLimit) {
    const doc_vision::Quad tilted{{{430, 160}, {850, 160}, {1180, 660}, {100, 660}}};
    ASSERT_GT(doc_vision::tiltDegrees(tilted, kFrameSize), 25.0);
    auto script = std::make_shared<Script>();
    script->steps = {page(tilted)};
    DocumentProcessor processor(scripted(script));
    const cv::Mat frame = test_support::pageInFrame(test_support::proceduralPage(), tilted, kFrameSize);

    const ProcessorResult result = processor.process(frameAt(frame, 0));

    const Finding* tilt = findingNamed(result, "Tilt");
    ASSERT_NE(tilt, nullptr);
    EXPECT_EQ(tilt->tone, Finding::Tone::Bad);
    EXPECT_NE(tilt->value.find("\xC2\xB0 (max 25)"), std::string::npos) << tilt->value;
}

// A page still failing inside the band is told the edge it must now pass, not the one it already crossed.
TEST(DocumentProcessor, RecoveringSharpnessShowsThePassAgainEdge) {
    const cv::Mat sharp = goodFrame();
    const double measured = doc_vision::measure(sharp, test_support::kPageQuad).sharpness;
    doc_vision::Thresholds thresholds;
    thresholds.sharpness = {measured - 10, measured + 10};
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script), thresholds);
    cv::Mat blurred;
    cv::GaussianBlur(sharp, blurred, {0, 0}, 8.0);

    processor.process(frameAt(blurred, 0));
    const ProcessorResult recovering = processor.process(frameAt(sharp, 1));

    const Finding* sharpness = findingNamed(recovering, "Sharpness");
    ASSERT_NE(sharpness, nullptr);
    EXPECT_EQ(sharpness->tone, Finding::Tone::Bad);
    EXPECT_NE(
        sharpness->value.find("(min " + std::to_string(static_cast<long>(std::ceil(measured + 10))) + ")"),
        std::string::npos)
        << sharpness->value;
}

// "22° (max 22)" reads as within the limit: a failing value is rounded away from passing, its limit towards
// it.
std::pair<long, long> valueAndLimit(const std::string& text) {
    const std::size_t open = text.find('(');
    const std::size_t digits = text.find_first_of("0123456789", open);
    return {std::stol(text), std::stol(text.substr(digits))};
}

TEST(DocumentProcessor, FailingTiltNeverShowsItsOwnLimit) {
    const doc_vision::Quad tilted{{{430, 160}, {850, 160}, {1180, 660}, {100, 660}}};
    const double tilt = doc_vision::tiltDegrees(tilted, kFrameSize);
    doc_vision::Thresholds thresholds;
    thresholds.tilt = {std::round(tilt) - 0.49, std::round(tilt) - 3}; // fails, and rounds to the same number
    ASSERT_GT(tilt, thresholds.tilt.failsAt);
    auto script = std::make_shared<Script>();
    script->steps = {page(tilted)};
    DocumentProcessor processor(scripted(script), thresholds);

    const ProcessorResult result = processor.process(
        frameAt(test_support::pageInFrame(test_support::proceduralPage(), tilted, kFrameSize), 0));

    const Finding* finding = findingNamed(result, "Tilt");
    ASSERT_NE(finding, nullptr);
    ASSERT_EQ(finding->tone, Finding::Tone::Bad);
    const auto [value, limit] = valueAndLimit(finding->value);
    EXPECT_GT(value, limit) << finding->value;
}

TEST(DocumentProcessor, FailingSharpnessNeverShowsItsOwnLimit) {
    const cv::Mat frame = goodFrame();
    const double sharpness = doc_vision::measure(frame, test_support::kPageQuad).sharpness;
    doc_vision::Thresholds thresholds;
    thresholds.sharpness = {std::round(sharpness) + 0.49, std::round(sharpness) + 3};
    ASSERT_LT(sharpness, thresholds.sharpness.failsAt);
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script), thresholds);

    const ProcessorResult result = processor.process(frameAt(frame, 0));

    const Finding* finding = findingNamed(result, "Sharpness");
    ASSERT_NE(finding, nullptr);
    ASSERT_EQ(finding->tone, Finding::Tone::Bad);
    const auto [value, limit] = valueAndLimit(finding->value);
    EXPECT_LT(value, limit) << finding->value;
}

TEST(DocumentProcessor, DarkPageIsTooDark) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));
    cv::Mat dark;
    goodFrame().convertTo(dark, -1, 0.25);

    const ProcessorResult result = processor.process(frameAt(dark, 0));

    EXPECT_EQ(*findingNamed(result, "Exposure"), (Finding{"Exposure", "too dark", Finding::Tone::Bad}));
}

// Review Focus 5: a seek, restart or loop shows the next hint at once.
TEST(DocumentProcessor, ResetShowsTheNextHintAtOnce) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));
    const cv::Mat frame = goodFrame();
    Hint shown = Hint::NoPage;
    for (int n = 0; n < 40 && shown != Hint::Good; ++n) {
        shown = processor.process(frameAt(frame, n)).guidance->shownHint;
    }
    ASSERT_EQ(shown, Hint::Good);

    processor.reset();
    script->steps = {page(kCutQuad)};
    script->calls = 0;
    const cv::Mat cut = test_support::pageInFrame(test_support::proceduralPage(), kCutQuad, kFrameSize);
    const frame_io::Frame later{.image = cut, .number = 0, .at = std::chrono::milliseconds{5000}, .tags = {}};

    EXPECT_EQ(processor.process(later).guidance->shownHint, Hint::CutByEdge);
}

// Review Focus 4: a model that will not open is reported on every frame of the run, and tried once per run.
TEST(DocumentProcessor, UnavailableModelReportedOnEveryFrameOnce) {
    int calls = 0;
    DocumentProcessor processor([&calls](std::string& error, doc_vision::OpenFailure& failure) {
        ++calls;
        error = "missing";
        failure = doc_vision::OpenFailure::Model;
        return std::unique_ptr<doc_vision::PageFinder>{};
    });
    const cv::Mat frame = goodFrame();

    for (int n = 0; n < 3; ++n) {
        const ProcessorResult result = processor.process(frameAt(frame, n));
        ASSERT_TRUE(result.guidance) << n;
        EXPECT_EQ(result.guidance->unavailable, Guidance::Unavailable::Model) << n;
        EXPECT_EQ(result.guidance->reason, "missing") << n;
        EXPECT_EQ(result.guidance->shownHint, Hint::NoPage) << n;
        EXPECT_FALSE(result.guidance->chips.visible) << n;
        EXPECT_TRUE(result.overlays.empty()) << n;
        EXPECT_EQ(findingNamed(result, "Detector")->tone, Finding::Tone::Bad) << n;
    }
    EXPECT_EQ(calls, 1);

    processor.reset();
    processor.process(frameAt(frame, 3));
    EXPECT_EQ(calls, 2);
}

TEST(DocumentProcessor, UnavailableRuntimeIsReportedAsSuch) {
    DocumentProcessor processor([](std::string& error, doc_vision::OpenFailure& failure) {
        error = "no runtime";
        failure = doc_vision::OpenFailure::Runtime;
        return std::unique_ptr<doc_vision::PageFinder>{};
    });

    const ProcessorResult result = processor.process(frameAt(goodFrame(), 0));

    ASSERT_TRUE(result.guidance);
    EXPECT_EQ(result.guidance->unavailable, Guidance::Unavailable::Runtime);
    EXPECT_EQ(result.guidance->reason, "no runtime");
}

// Review Focus 3: a 16-bit frame is scaled by its value range, not shown black.
TEST(DocumentProcessor, SixteenBitFrameGuidesAsItsEightBitTwin) {
    const cv::Mat eight = goodFrame();
    cv::Mat sixteen;
    eight.convertTo(sixteen, CV_16UC3, 257.0);
    auto script8 = std::make_shared<Script>();
    script8->steps = {page(test_support::kPageQuad)};
    auto script16 = std::make_shared<Script>();
    script16->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor8(scripted(script8));
    DocumentProcessor processor16(scripted(script16));

    for (int n = 0; n < 20; ++n) {
        const ProcessorResult a = processor8.process(frameAt(eight, n));
        const ProcessorResult b = processor16.process(frameAt(sixteen, n, {.valueRange = {0, 65535}}));
        ASSERT_TRUE(a.guidance && b.guidance);
        EXPECT_EQ(a.guidance->shownHint, b.guidance->shownHint) << n;
        EXPECT_EQ(tonesOf(a), tonesOf(b)) << n;
    }
    ASSERT_EQ(script16->lastImage.type(), CV_8UC3);
    EXPECT_EQ(cv::norm(script16->lastImage, eight, cv::NORM_INF), 0.0);
}

TEST(DocumentProcessor, PageImageIsTheGatePage) {
    auto script = std::make_shared<Script>();
    script->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(script));

    const ProcessorResult result = processor.process(frameAt(goodFrame(), 0));

    ASSERT_EQ(result.images.size(), 1U);
    EXPECT_EQ(result.images[0].name, "page");
    EXPECT_EQ(result.images[0].image.size(), doc_vision::kGatePageSize);
    EXPECT_EQ(result.images[0].image.type(), CV_8UC3);
}

TEST(DocumentProcessor, MakeProcessorNamesIt) {
    EXPECT_EQ(lab_core::nameOf(lab_core::ProcessorKind::Document), "Document");
}

// P23: the real LC100 finds the procedural page.
TEST(DocumentProcessor, RealModelEndToEnd) {
    const std::filesystem::path model = ID_VISION_LAB_LC100_MODEL;
    ASSERT_EQ(model.filename(), lab_core::kLc100ModelFile);
    const auto processor =
        lab_core::makeProcessor(lab_core::ProcessorKind::Document, {.modelFolder = model.parent_path()});

    const ProcessorResult result = processor->process(frameAt(goodFrame(), 0));

    ASSERT_TRUE(result.guidance);
    ASSERT_EQ(result.guidance->unavailable, Guidance::Unavailable::None) << result.guidance->reason;
    const visual_debug::Polygon& outline = outlineOf(result);
    ASSERT_EQ(outline.points.size(), 4U);
    const doc_vision::Quad found{outline.points[0], outline.points[1], outline.points[2], outline.points[3]};
    EXPECT_GT(test_support::quadIoU(found, test_support::kPageQuad), 0.9);
}

} // namespace
