#include "lab_core/document_processor.h"

#include "number_text.h"

#include <doc_vision/mrz.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
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
    findings.push_back({"Document", "found", Tone::Plain}); // which kind waits for the MRZ (A4)
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
    return {{"Document", "not found", Tone::Plain}, {"Gates", "no document yet", Tone::Plain}};
}

// The rows of a page's own measures, kept with a captured frame for Review's gate values.
std::vector<Finding> gateRows(const std::vector<Finding>& findings) {
    std::vector<Finding> rows;
    for (const Finding& finding : findings) {
        if (finding.name == "Fill" || finding.name == "Tilt" || finding.name == "Sharpness" ||
            finding.name == "Glare" || finding.name == "Exposure") {
            rows.push_back(finding);
        }
    }
    return rows;
}

double seconds(std::chrono::nanoseconds time) { return std::chrono::duration<double>(time).count(); }

// Screen 04's Format row: the MRZ's size and what it says the document is (A4.1, spec §6.2); only a passport
// is accepted in TD3's size (Mrz::supported).
std::string formatRow(const doc_vision::Mrz& mrz) {
    switch (mrz.format) {
    case doc_vision::MrzFormat::Td1:
        return "TD1 ID card";
    case doc_vision::MrzFormat::Td2:
        return "TD2 ID card";
    case doc_vision::MrzFormat::Td3:
        break;
    }
    return "TD3 passport";
}

} // namespace

DocumentProcessor::DocumentProcessor(MakeFinder makeFinder, doc_vision::Thresholds thresholds, Reader reader)
    : m_makeFinder(std::move(makeFinder)), m_judge(thresholds) {
    if (reader.make) {
        m_worker = std::make_unique<MrzWorker>(reader.mode, std::move(reader.make));
    }
}

bool DocumentProcessor::pumpReaderForTests() { return m_worker && m_worker->pump(); }

// The reads finished since the last frame: an older reset's dropped, an error only shown, a valid read of a
// supported document added to the agreement with its frame kept, an accept made into the scan.
void DocumentProcessor::collectReads() {
    for (MrzDone& done : m_worker->take()) {
        if (done.resetCount != m_resetCount || m_scan) {
            continue;
        }
        if (!done.error.empty()) {
            m_lastReadError = done.error;
            continue;
        }
        m_lastReadError.clear();
        ++m_reads;
        m_lastReadTime = done.reading.detectTime + done.reading.recogniseTime;
        if (!done.reading.band) {
            m_lastReadNoBand = true; // hint 11
            m_band.reset();
            continue;
        }
        m_lastReadNoBand = false;
        m_band = BandSeen{*done.reading.band, done.turned};
        m_turned = done.turned;
        const std::optional<doc_vision::Mrz> mrz = doc_vision::parseMrz(done.reading.lines);
        const std::uint64_t number = done.record.frame.number;
        if (!mrz || !mrz->valid() || !mrz->supported() || m_agreement.holds(number)) {
            continue;
        }
        std::string text = mrz->text();
        const auto same =
            std::find_if(m_kept.begin(), m_kept.end(), [&](const Kept& k) { return k.text == text; });
        // The sharpest read's frame per text; of two as sharp, the later.
        if (same == m_kept.end()) {
            m_kept.push_back({text, done.record, *done.reading.band, done.turned});
        } else if (done.record.sharpness >= same->record.sharpness) {
            *same = {text, done.record, *done.reading.band, done.turned};
        }
        const std::optional<std::string> agreed = m_agreement.add(std::move(text), number);
        std::erase_if(m_kept, [this](const Kept& k) { return !m_agreement.holds(k.record.frame.number); });
        if (agreed) {
            accept(*mrz, done);
            return;
        }
    }
}

void DocumentProcessor::accept(const doc_vision::Mrz& mrz, const MrzDone& accepting) {
    const std::string text = mrz.text();
    const auto kept =
        std::find_if(m_kept.begin(), m_kept.end(), [&](const Kept& k) { return k.text == text; });
    if (kept == m_kept.end()) {
        return; // cannot happen: the accepting read's text is kept
    }
    MrzScan scan;
    scan.frame = kept->record.frame;
    const doc_vision::Quad& quad = kept->record.quad;
    const doc_vision::Quad bandOnPage =
        kept->turned ? doc_vision::unturned(kept->bandOnPage, doc_vision::kPageSize) : kept->bandOnPage;
    const doc_vision::Quad band = doc_vision::pageToFrame(bandOnPage, quad, doc_vision::kPageSize);
    // The right way up: a page read turned has its top left at the flattened page's bottom right. The band is
    // already in its order as read (the reader's corners on the page it read), and mapping keeps that order.
    const std::size_t shift = kept->turned ? 2 : 0;
    for (std::size_t i = 0; i < 4; ++i) {
        scan.quad[i] = quad[(i + shift) % 4];
    }
    scan.band = band;
    scan.gates = kept->record.gates;
    scan.mrz = mrz;
    scan.timeToScan = accepting.record.frame.at - accepting.record.firstPageAt;
    scan.reads = m_reads;
    m_scan = std::make_shared<const MrzScan>(std::move(scan));
    m_kept.clear();
    m_agreement.clear();
}

// Reads still coming belong to the window before: dropped by their count, as after a reset.
void DocumentProcessor::clearReadWindow() {
    ++m_resetCount;
    if (m_worker) {
        m_worker->forget(m_resetCount);
    }
    m_agreement.clear();
    m_kept.clear();
    m_reads = 0;
    m_firstPageAt.reset();
    m_firstGatePassAt.reset();
    m_lastFreshAt.reset();
    m_lastReadNoBand = false;
    m_band.reset();
}

std::vector<Finding> DocumentProcessor::readerFindings() const {
    if (m_worker->unavailable()) {
        return {{"Reader", "unavailable", Tone::Bad}};
    }
    if (!m_lastReadError.empty()) {
        return {{"Reader", "error: " + m_lastReadError, Tone::Bad}};
    }
    if (!m_lastReadTime) {
        return {{"Reader", "MRZScanner · –", Tone::Plain}};
    }
    const std::chrono::duration<double, std::milli> took = *m_lastReadTime;
    return {{"Reader", "MRZScanner · " + oneDecimal(took.count()) + " ms", Tone::Plain}};
}

// Screen 04's rows.
std::vector<Finding> DocumentProcessor::scanFindings() const {
    std::size_t passed = 0;
    for (const doc_vision::MrzCheckResult& check : m_scan->mrz.checks) {
        passed += check.passed ? 1 : 0;
    }
    return {{"MRZ", "accepted, 3 of 3", Tone::Good},
            {"Format", formatRow(m_scan->mrz), Tone::Plain},
            {"Check digits",
             wholeNumber(static_cast<double>(passed)) + " of " +
                 wholeNumber(static_cast<double>(m_scan->mrz.checks.size())) + " pass",
             Tone::Plain},
            {"Time to scan", oneDecimal(seconds(m_scan->timeToScan)) + " s", Tone::Plain},
            {"Captured", "frame " + wholeNumber(static_cast<double>(m_scan->frame.number + 1)), Tone::Plain}};
}

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
            m_reason = error.empty() ? "the document finder could not be opened" : std::move(error);
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

    if (m_worker) {
        collectReads();
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
        // Which way up a page flattens follows its corners' history: a turn is forgotten with it.
        m_turned = false;
        // A4.1: the card left the detector's view for longer than PageHold bridges: it may have been turned.
        m_frontTurned = true;
    }
    // Nothing is read once a scan is accepted, nor in the front stage, which has no MRZ to read (A4.1).
    const bool reading = m_worker && !m_scan && !m_frontStage && !m_worker->unavailable();
    if (m_worker) {
        if (facts) {
            m_lastFreshAt = frame.at;
            if (!m_firstPageAt) {
                m_firstPageAt = frame.at;
            }
            if (facts->verdicts.allPassed() && !m_firstGatePassAt) {
                m_firstGatePassAt = frame.at;
            }
        } else if (m_lastFreshAt && frame.at - *m_lastFreshAt >= kReadWindowClearAfter) {
            clearReadWindow();
        }
    }
    const std::optional<doc_vision::Verdicts> verdicts =
        held ? std::optional<doc_vision::Verdicts>(held->verdicts) : std::nullopt;
    guidance.frameHint = doc_vision::hintFor(verdicts);
    // Every gate passes: the reader may say more. Hint 12 once its 5 s have passed, else hint 11.
    if (reading && guidance.frameHint == doc_vision::Hint::Good) {
        if (m_firstGatePassAt && frame.at - *m_firstGatePassAt >= kCannotReadAfter) {
            guidance.frameHint = doc_vision::Hint::CannotReadMrz;
        } else if (m_lastReadNoBand) {
            guidance.frameHint = doc_vision::Hint::NoMrzBand;
        }
    }
    if (facts) {
        facts->frameHint = guidance.frameHint;
        guidance.page = std::move(facts);
    }
    guidance.shownHint = m_hintTimer.next(guidance.frameHint, frame.at);
    guidance.chips = m_chipTimer.next(verdicts, guidance.shownHint, frame.at);
    std::optional<doc_vision::Quad> bandInFrame;

    if (held) {
        guidance.outlineOverlay = result.overlays.size();
        result.overlays.push_back(visual_debug::Polygon{.points = {held->quad.begin(), held->quad.end()},
                                                        .label = "Document",
                                                        .colour = guidance.shownHint == doc_vision::Hint::Good
                                                                      ? visual_debug::kOverlayGreen
                                                                      : visual_debug::kOverlayRed});
        result.images.push_back({.name = "document", .image = m_page, .description = {}});
        result.findings = pageFindings(m_measures, held->verdicts, m_judge.thresholds());
        // The last read's band, through this frame's own quad so it moves with the outline: only on a page
        // found in this frame, and while the banner says Good or hint 12 (spec §3.2).
        const bool bannerAllows = guidance.shownHint == doc_vision::Hint::Good ||
                                  guidance.shownHint == doc_vision::Hint::CannotReadMrz;
        if (reading && m_band && guidance.page && bannerAllows) {
            const doc_vision::Quad onPage =
                m_band->turned ? doc_vision::unturned(m_band->onPage, doc_vision::kPageSize) : m_band->onPage;
            const doc_vision::Quad band =
                doc_vision::pageToFrame(onPage, guidance.page->quad, doc_vision::kPageSize);
            guidance.bandOverlay = result.overlays.size();
            result.overlays.push_back(visual_debug::Polygon{.points = {band.begin(), band.end()},
                                                            .label = "MRZ band",
                                                            .colour = visual_debug::kOverlayGreen});
            bandInFrame = band;
        }
    } else {
        result.findings = noPageFindings();
    }
    if (error.empty()) {
        result.findings.push_back({"Detector", "LC100 · " + oneDecimal(took.count()) + " ms", Tone::Plain});
    } else {
        result.findings.push_back({"Detector", "error: " + error, Tone::Bad});
    }

    if (m_frontStage) {
        // After the turn, the first frame whose banner has steadied on Good and whose own page, found in it,
        // passes every gate: a new rule, stricter than "the first gate-passing frame" (spec §6.4).
        const std::optional<PageFacts>& fresh = guidance.page;
        if (m_frontTurned && !m_front && guidance.shownHint == doc_vision::Hint::Good && fresh &&
            fresh->verdicts.allPassed()) {
            m_front = std::make_shared<const FrontShot>(
                FrontShot{.frame = frame, .quad = fresh->quad, .gates = gateRows(result.findings)});
        }
        guidance.frontStage = m_frontTurned ? FrontStage::Looking : FrontStage::AwaitingTurn;
        guidance.front = m_front;
    }

    if (m_worker) {
        const std::optional<PageFacts>& fresh = guidance.page;
        // A frame whose page, found in it, passes every gate: its page is read on the worker's thread.
        if (reading && fresh && fresh->verdicts.allPassed()) {
            MrzJob job;
            job.bgr = bgr;
            job.record = {.frame = frame,
                          .quad = fresh->quad,
                          .sharpness = fresh->sharpness,
                          .gates = gateRows(result.findings),
                          .firstPageAt = m_firstPageAt.value_or(frame.at)};
            job.turnFirst = m_turned;
            job.resetCount = m_resetCount;
            m_worker->submit(std::move(job));
        }
        if (m_scan) {
            guidance.scan = m_scan;
            if (!m_frontStage) {
                result.findings = scanFindings(); // in the front stage, the page's as the user adjusts it
            }
        } else if (!m_frontStage) {
            MrzProgress progress;
            if (const std::optional<std::string> why = m_worker->unavailable()) {
                progress.available = false;
                progress.reason = *why;
            }
            progress.agreeing = m_agreement.count();
            progress.band = bandInFrame;
            guidance.reading = progress;
            if (held) {
                result.findings.push_back(
                    {"MRZ reads", wholeNumber(static_cast<double>(progress.agreeing)) + " of 3 agree",
                     Tone::Plain});
            }
            const std::vector<Finding> reader = readerFindings();
            result.findings.insert(result.findings.end(), reader.begin(), reader.end());
        }
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
    // Reads still coming are of the run before: dropped by their count. The reader stays open. The frames it
    // still holds go now: no process() may come to take them (Stop, Discard).
    ++m_resetCount;
    if (m_worker) {
        m_worker->forget(m_resetCount);
    }
    m_agreement.clear();
    m_kept.clear();
    m_reads = 0;
    m_turned = false;
    m_firstPageAt.reset();
    m_lastReadTime.reset();
    m_lastReadError.clear();
    m_scan.reset();
    m_lastFreshAt.reset();
    m_firstGatePassAt.reset();
    m_lastReadNoBand = false;
    m_band.reset();
    // The front stage stays (spec §6.4): the frames before a reset (a new source, a seek, a loop) are
    // forgotten, which counts as the card's turn. The hint timer starts afresh and shows its first hint at
    // once, so the first frame after a reset whose page passes every gate may be kept as the front.
    m_frontTurned = true;
    m_front.reset();
}

void DocumentProcessor::setFrontStage(bool on) {
    m_frontStage = on;
    m_frontTurned = false;
    m_front.reset();
}

} // namespace lab_core
