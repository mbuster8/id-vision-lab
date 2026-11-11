#include "lab_core/document_processor.h"

#include "number_text.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace lab_core {

namespace {

using Tone = Finding::Tone;

constexpr std::size_t gateIndex(doc_vision::Gate gate) { return static_cast<std::size_t>(gate); }

// The edge a failing measure must reach now: the one it fell past, or, while it recovers inside the band, the
// edge where it passes again.
double edgeToReach(double value, const doc_vision::Band& band, bool failsWhenLow) {
    const bool pastFailEdge = failsWhenLow ? value < band.failsAt : value > band.failsAt;
    return pastFailEdge ? band.failsAt : band.passesAgainAt;
}

std::vector<Finding> pageFindings(const doc_vision::Measures& m, const doc_vision::Verdicts& verdicts,
                                  const doc_vision::Thresholds& thresholds) {
    std::vector<Finding> findings;
    findings.push_back({"Document", "TD3 page", Tone::Plain});
    findings.push_back({"Fill", wholeNumber(100 * m.fill) + " %", Tone::Plain});
    if (verdicts.passed[gateIndex(doc_vision::Gate::Flat)]) {
        findings.push_back({"Tilt", wholeNumber(m.tiltDegrees) + "°", Tone::Plain});
    } else {
        // Rounded away from passing, the limit towards it, so a failing value never reads as its own limit.
        findings.push_back({"Tilt",
                            wholeNumber(std::ceil(m.tiltDegrees)) + "° (max " +
                                wholeNumber(std::floor(edgeToReach(m.tiltDegrees, thresholds.tilt, false))) +
                                ")",
                            Tone::Bad});
    }
    if (verdicts.passed[gateIndex(doc_vision::Gate::Sharp)]) {
        findings.push_back({"Sharpness", wholeNumber(m.sharpness), Tone::Plain});
    } else {
        findings.push_back({"Sharpness",
                            wholeNumber(std::floor(m.sharpness)) + " (min " +
                                wholeNumber(std::ceil(edgeToReach(m.sharpness, thresholds.sharpness, true))) +
                                ")",
                            Tone::Bad});
    }
    findings.push_back({"Glare", oneDecimal(100 * m.glareShare) + " %",
                        verdicts.passed[gateIndex(doc_vision::Gate::Glare)] ? Tone::Plain : Tone::Bad});
    if (verdicts.tooDark) {
        findings.push_back({"Exposure", "too dark", Tone::Bad});
    } else if (verdicts.tooBright) {
        findings.push_back({"Exposure", "too bright", Tone::Bad});
    } else {
        findings.push_back(
            {"Exposure", "ok, " + oneDecimal(100 * m.clippedShare) + " % clipped", Tone::Plain});
    }
    std::size_t passed = 0;
    for (const bool gate : verdicts.passed) {
        passed += gate ? 1 : 0;
    }
    if (passed == doc_vision::kGateCount) {
        findings.push_back({"Gates", "all pass", Tone::Good});
    } else {
        findings.push_back({"Gates",
                            wholeNumber(static_cast<double>(passed)) + " of " +
                                wholeNumber(static_cast<double>(doc_vision::kGateCount)) + " pass",
                            Tone::Bad});
    }
    return findings;
}

std::vector<Finding> noPageFindings() {
    return {{"Document", "not found", Tone::Plain}, {"Gates", "no page yet", Tone::Plain}};
}

} // namespace

DocumentProcessor::DocumentProcessor(MakeFinder makeFinder, doc_vision::Thresholds thresholds)
    : m_makeFinder(std::move(makeFinder)), m_judge(thresholds) {}

ProcessorResult DocumentProcessor::process(const frame_io::Frame& frame) {
    // Opened lazily, so a run that never picks the Document processor never loads the model; tried once per
    // run.
    if (!m_finder && m_unavailable == Guidance::Unavailable::None) {
        std::string error;
        doc_vision::OpenFailure failure = doc_vision::OpenFailure::None;
        if (m_makeFinder) {
            m_finder = m_makeFinder(error, failure);
        }
        if (!m_finder) {
            m_unavailable = failure == doc_vision::OpenFailure::Runtime ? Guidance::Unavailable::Runtime
                                                                        : Guidance::Unavailable::Model;
            m_reason = error.empty() ? "the page finder could not be opened" : std::move(error);
        }
    }

    ProcessorResult result;
    Guidance guidance;
    if (!m_finder) {
        guidance.unavailable = m_unavailable;
        guidance.reason = m_reason;
        result.findings = noPageFindings();
        result.findings.push_back({"Detector", "unavailable", Tone::Bad});
        result.guidance = std::move(guidance);
        return result;
    }

    const visual_debug::ValueRange range =
        frame.tags.valueRange.empty() ? visual_debug::ValueRange{} : frame.tags.valueRange;
    const cv::Mat bgr = doc_vision::toBgr8(frame.image, range.min, range.max);

    std::string error;
    const auto started = std::chrono::steady_clock::now();
    const std::optional<doc_vision::PageFound> found = m_finder->find(bgr, error);
    const std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - started;

    std::optional<std::pair<doc_vision::Quad, doc_vision::Verdicts>> judged;
    std::optional<PageFacts> facts;
    if (found) {
        const doc_vision::Quad quad =
            m_previous ? doc_vision::alignTo(found->quad, *m_previous) : found->quad;
        cv::Mat page; // a new one each time: the last result's picture may still be on its way
        m_measures = doc_vision::measure(bgr, quad, &page);
        m_page = std::move(page);
        judged.emplace(quad, m_judge.judge(m_measures));
        m_previous = quad;
        facts = PageFacts{.quad = quad,
                          .frameSize = bgr.size(),
                          .sharpness = m_measures.sharpness,
                          .longerSide = m_measures.longerSide,
                          .verdicts = judged->second,
                          .frameHint = doc_vision::Hint::NoPage};
    }
    const std::optional<doc_vision::Held> held = m_hold.next(judged, frame.at);
    if (!held) {
        // A page found again is judged afresh, not by the bands of one seen before.
        m_judge.reset();
        m_previous.reset();
        m_page = cv::Mat();
    }
    const std::optional<doc_vision::Verdicts> verdicts =
        held ? std::optional<doc_vision::Verdicts>(held->verdicts) : std::nullopt;
    guidance.frameHint = doc_vision::hintFor(verdicts);
    if (facts) {
        facts->frameHint = guidance.frameHint;
        guidance.page = std::move(facts);
    }
    guidance.shownHint = m_hintTimer.next(guidance.frameHint, frame.at);
    guidance.chips = m_chipTimer.next(verdicts, guidance.shownHint, frame.at);

    if (held) {
        guidance.outlineOverlay = result.overlays.size();
        result.overlays.push_back(visual_debug::Polygon{.points = {held->quad.begin(), held->quad.end()},
                                                        .label = "Document",
                                                        .colour = guidance.shownHint == doc_vision::Hint::Good
                                                                      ? visual_debug::kOverlayGreen
                                                                      : visual_debug::kOverlayRed});
        result.images.push_back({.name = "page", .image = m_page, .description = {}});
        result.findings = pageFindings(m_measures, held->verdicts, m_judge.thresholds());
    } else {
        result.findings = noPageFindings();
    }
    if (error.empty()) {
        result.findings.push_back({"Detector", "LC100 · " + oneDecimal(took.count()) + " ms", Tone::Plain});
    } else {
        result.findings.push_back({"Detector", "error: " + error, Tone::Bad});
    }
    result.guidance = std::move(guidance);
    return result;
}

void DocumentProcessor::reset() {
    m_judge.reset();
    m_hintTimer.reset();
    m_chipTimer.reset();
    m_hold.reset();
    m_previous.reset();
    m_measures = {};
    m_page = cv::Mat();
    if (!m_finder) {
        m_unavailable = Guidance::Unavailable::None;
        m_reason.clear();
    }
}

} // namespace lab_core
