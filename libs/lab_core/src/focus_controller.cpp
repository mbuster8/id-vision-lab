#include "lab_core/focus_controller.h"

#include "number_text.h"

#include <doc_vision/guidance.h>
#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace lab_core {

namespace {

using std::chrono::nanoseconds;

// How far, in pixels, any corner of `to` is from where it was in `from`, the corners first matched as the
// outline matches them (a quad found again may start at the opposite corner).
double cornerDrift(const doc_vision::Quad& from, const doc_vision::Quad& to) {
    const doc_vision::Quad aligned = doc_vision::alignTo(to, from);
    double drift = 0.0;
    for (std::size_t i = 0; i < aligned.size(); ++i) {
        drift = std::max(drift, cv::norm(aligned[i] - from[i]));
    }
    return drift;
}

bool sizeChanged(double side, double reference) {
    return reference > 0.0 && std::abs(side - reference) / reference > kSizeChange;
}

bool passes(const PageFacts& page, doc_vision::Gate gate) {
    return page.verdicts.passed[static_cast<std::size_t>(gate)];
}

double seconds(nanoseconds duration) { return std::chrono::duration<double>(duration).count(); }

} // namespace

Finding focusFinding(const FocusStatus& status) {
    switch (status.state) {
    case FocusStatus::State::Searching:
        return {"Focus", "lab searching", Finding::Tone::Plain};
    case FocusStatus::State::Holding:
        return {"Focus", "lab, " + std::to_string(status.position.value_or(0)), Finding::Tone::Plain};
    case FocusStatus::State::CannotFocus:
        return {"Focus", "can't focus", Finding::Tone::Bad};
    case FocusStatus::State::Camera:
        break;
    }
    return {"Focus", status.controllable ? "camera" : "camera (lab can't set it)", Finding::Tone::Plain};
}

FocusController::FocusController(std::unique_ptr<FocusControl> control, NextOffer nextOffer, double passesAt)
    : m_control(std::move(control)), m_nextOffer(std::move(nextOffer)), m_passesAt(passesAt) {}

FocusController::~FocusController() { end(); }

FocusStatus FocusController::next(const frame_io::Frame& frame, const std::optional<PageFacts>& page) {
    try {
        step(frame, page);
    } catch (...) {
        // Nothing leaves the processing thread: the status stays as it was.
    }
    m_status.controllable = canFocus();
    m_status.blurredFirstSince = m_blurredSince;
    return m_status;
}

void FocusController::requestAt(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    m_request = std::pair{std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)};
}

bool FocusController::end() noexcept {
    try {
        m_search.reset();
        return restore();
    } catch (...) {
        return false;
    }
}

void FocusController::step(const frame_io::Frame& frame, const std::optional<PageFacts>& page) {
    checkControl();
    watch(frame, page);
    const nanoseconds at = frame.at;
    if (m_request) {
        const auto [x, y] = *m_request;
        m_request.reset();
        if (!canFocus()) {
            setNote("This camera's focus can't be set from the lab");
        } else {
            // A click always wins: it ends any search and starts its own.
            startClickSearch(frame, x, y);
            return;
        }
    }
    if (m_search) {
        searchFrame(frame, page);
        return;
    }
    switch (m_status.state) {
    case FocusStatus::State::CannotFocus:
        if (lost(at) || (page && (sizeChanged(page->longerSide, m_cannotSide) ||
                                  passes(*page, doc_vision::Gate::Sharp)))) {
            m_status.state = FocusStatus::State::Camera;
            m_status.afterSearch = false;
            m_blurredSince.reset();
        }
        break;
    case FocusStatus::State::Camera:
        if (canFocus()) {
            if (page && mayStartPageSearch(at)) {
                startPageSearch(frame, *page, false);
            } else if (m_memory && m_memory->failed && steady() && m_blurredSince &&
                       at - *m_blurredSince >= kCannotFocusAfter) {
                // A search already failed for this page: a noisy sharp frame does not lose "can't focus".
                m_status.state = FocusStatus::State::CannotFocus;
                m_status.afterSearch = true;
                m_cannotSide = m_lastSide;
            }
        } else if (steady() && m_blurredSince && at - *m_blurredSince >= kCannotFocusAfter) {
            m_status.state = FocusStatus::State::CannotFocus;
            m_status.afterSearch = false;
            m_cannotSide = m_lastSide;
        }
        break;
    case FocusStatus::State::Holding:
        if (m_status.kind == FocusStatus::Kind::Page) {
            // The hold's loss clock starts with the hold: a search aborted by a lost page holds its best for
            // another second, waiting for the page.
            const nanoseconds seen = m_lastFreshAt ? std::max(*m_lastFreshAt, m_heldSince) : m_heldSince;
            if (at - seen >= kLostAfter || (page && sizeChanged(page->longerSide, m_heldSide))) {
                letGo();
                break;
            }
            // The page's searches have run out and it is blurred again: the held position no longer works, so
            // the camera's own focus gets its turn, and "can't focus" may follow.
            if (page && m_memory && m_memory->searches >= kSearchesPerMemory && steady() && m_blurredSince &&
                at - *m_blurredSince >= kTakeOverAfter &&
                (!m_lastSearchEnd || at - *m_lastSearchEnd >= kCoolDown)) {
                letGo();
                m_memory->blocked = true;
                m_memory->failed = true;
                m_blurredSince.reset();
                break;
            }
        }
        if (page && mayStartPageSearch(at)) {
            startPageSearch(frame, *page, m_status.kind == FocusStatus::Kind::Page);
        }
        break;
    case FocusStatus::State::Searching:
        break;
    }
}

void FocusController::checkControl() {
    if (m_checked) {
        return;
    }
    m_checked = true;
    if (!m_control) {
        return;
    }
    // Asked once, on the run's first frame: a control without a range is no control for the whole run.
    const std::optional<FocusRange> range = m_control->range();
    if (range && range->maximum > range->minimum && range->step >= 1) {
        m_range = range;
    }
}

// The steadiness window, the "Blurred first" timer and the page memory, from each frame.
void FocusController::watch(const frame_io::Frame& frame, const std::optional<PageFacts>& page) {
    const nanoseconds at = frame.at;
    if (page) {
        m_window.push_back(
            {.at = at,
             .quad = page->quad,
             .frameSide = static_cast<double>(std::max(page->frameSize.width, page->frameSize.height))});
        m_lastFreshAt = at;
        m_lastSide = page->longerSide;
        if (page->frameHint == doc_vision::Hint::Blurred) {
            if (!m_blurredSince) {
                m_blurredSince = at;
            }
        } else {
            m_blurredSince.reset();
        }
        if (m_memory && sizeChanged(page->longerSide, m_memory->side)) {
            m_memory.reset();
        }
    } else {
        // A frame without a fresh page neither breaks nor extends the timers, unless the page is lost.
        m_window.push_back({.at = at, .quad = std::nullopt, .frameSide = 0.0});
        if (lost(at)) {
            m_blurredSince.reset();
            m_memory.reset();
        }
    }
    while (!m_window.empty() && at - m_window.front().at > kSteadyWindow) {
        m_window.pop_front();
    }
}

bool FocusController::steady() const {
    if (m_window.size() < 2) {
        return false;
    }
    std::size_t fresh = 0;
    const Seen* first = nullptr;
    double drift = 0.0;
    for (const Seen& seen : m_window) {
        if (!seen.quad) {
            continue;
        }
        ++fresh;
        if (first == nullptr) {
            first = &seen;
        } else {
            drift = std::max(drift, cornerDrift(*first->quad, *seen.quad) / seen.frameSide);
        }
    }
    return first != nullptr &&
           static_cast<double>(fresh) >= kSteadyFound * static_cast<double>(m_window.size()) &&
           drift <= kSteadyCorners;
}

bool FocusController::lost(nanoseconds at) const {
    return !m_lastFreshAt || at - *m_lastFreshAt >= kLostAfter;
}

bool FocusController::mayStartPageSearch(nanoseconds at) const {
    return steady() && m_blurredSince && at - *m_blurredSince >= kTakeOverAfter &&
           (!m_lastSearchEnd || at - *m_lastSearchEnd >= kCoolDown) &&
           (!m_memory || (!m_memory->blocked && m_memory->searches < kSearchesPerMemory));
}

// Records what the camera's focus was before the lab's first change in the run, and turns its autofocus off.
bool FocusController::takeControl() {
    if (!m_found) {
        m_hasAuto = m_control->hasAutoFocus();
        FocusFound found;
        found.autoFocus = m_hasAuto && m_control->autoFocus().value_or(true);
        found.position = m_control->position();
        m_found = found;
    }
    if (m_hasAuto && !m_autoOff) {
        if (!m_control->setAutoFocus(false)) {
            giveUp();
            return false;
        }
        m_autoOff = true;
        m_changed = true;
    }
    return true;
}

void FocusController::startPageSearch(const frame_io::Frame& frame, const PageFacts& page, bool local) {
    const std::optional<int> held =
        m_status.state == FocusStatus::State::Holding ? m_status.position : std::nullopt;
    if (!takeControl()) {
        return;
    }
    const std::optional<int> lens = held ? held : m_control->position();
    Search search;
    search.kind = FocusStatus::Kind::Page;
    search.startedAt = frame.at;
    search.side = page.longerSide;
    search.lensFrom = lens.value_or(m_range->minimum);
    if (!m_memory) {
        m_memory = Memory{.side = page.longerSide, .searches = 0, .blocked = false};
    }
    ++m_memory->searches;
    m_curve.clear();
    m_status.state = FocusStatus::State::Searching;
    m_status.kind = FocusStatus::Kind::Page;
    m_status.afterSearch = false;
    m_search = search;
    if (local && held) {
        std::vector<int> around;
        for (int k = -kLocalSteps; k <= kLocalSteps; ++k) {
            const int position = *held + k * coarseStep();
            if (position >= m_range->minimum && position <= top()) {
                around.push_back(onGrid(position));
            }
        }
        beginStage(Stage::Local, std::move(around), frame);
    } else {
        const int start = predicted(page.longerSide).value_or(lens.value_or(m_range->minimum));
        beginStage(Stage::Coarse, coarsePositions(start), frame);
    }
}

void FocusController::startClickSearch(const frame_io::Frame& frame, double x, double y) {
    std::optional<int> lens;
    if (m_search) {
        lens = m_search->lensFrom;
    } else if (m_status.state == FocusStatus::State::Holding) {
        lens = m_status.position;
    }
    m_search.reset();
    if (!takeControl()) {
        return;
    }
    if (!lens) {
        lens = m_control->position();
    }
    const int width = frame.image.cols;
    const int height = frame.image.rows;
    const int side = std::max(1, static_cast<int>(std::lround(kClickRegionShare * std::min(width, height))));
    const int cx = static_cast<int>(std::lround(x * width));
    const int cy = static_cast<int>(std::lround(y * height));
    Search search;
    search.kind = FocusStatus::Kind::Click;
    search.startedAt = frame.at;
    search.region = cv::Rect(cx - side / 2, cy - side / 2, side, side) & cv::Rect(0, 0, width, height);
    search.before = clickValue(frame, search.region);
    search.lensFrom = lens.value_or(m_range->minimum);
    m_curve.clear();
    m_status.state = FocusStatus::State::Searching;
    m_status.kind = FocusStatus::Kind::Click;
    m_status.afterSearch = false;
    setNote("Focusing…");
    m_search = search;
    beginStage(Stage::Coarse, coarsePositions(search.lensFrom), frame);
}

// The region's sharpness, the crop first turned into 8-bit BGR by the frame's own value range (as the
// Document processor turns the whole frame).
double FocusController::clickValue(const frame_io::Frame& frame, cv::Rect region) const {
    if (frame.image.empty()) {
        return 0.0;
    }
    const cv::Rect inside = region & cv::Rect(0, 0, frame.image.cols, frame.image.rows);
    if (inside.empty()) {
        return 0.0;
    }
    const visual_debug::ValueRange range =
        frame.tags.valueRange.empty() ? visual_debug::ValueRange{} : frame.tags.valueRange;
    const cv::Mat crop = doc_vision::toBgr8(frame.image(inside), range.min, range.max);
    return doc_vision::regionSharpness(crop, cv::Rect(0, 0, crop.cols, crop.rows));
}

void FocusController::beginStage(Stage stage, std::vector<int> positions, const frame_io::Frame& frame) {
    Search& search = *m_search;
    search.stage = stage;
    search.positions = std::move(positions);
    search.index = 0;
    if (search.positions.empty()) {
        finishSearch(frame);
        return;
    }
    moveTo(search.positions.front(), frame);
}

// Moves the lens; only frames offered after this, and once the lens had time to settle, count for it.
bool FocusController::moveTo(int position, const frame_io::Frame& frame) {
    Search& search = *m_search;
    if (!m_control->setPosition(position)) {
        giveUp();
        return false;
    }
    m_changed = true;
    const int units = std::abs(position - search.lensFrom);
    search.lensFrom = position;
    search.settle = kSettleBase + kSettlePerUnit * std::max(0, units - kSettleFreeUnits);
    search.waitFrom = m_nextOffer ? m_nextOffer() : frame.number + 1;
    search.firstCountingAt.reset();
    search.samples.clear();
    m_status.position = position;
    m_status.countingFrom = search.waitFrom;
    return true;
}

void FocusController::searchFrame(const frame_io::Frame& frame, const std::optional<PageFacts>& page) {
    Search& search = *m_search;
    const nanoseconds at = frame.at;
    const bool pageSearch = search.kind == FocusStatus::Kind::Page;
    if ((pageSearch && (lost(at) || (page && (sizeChanged(page->longerSide, search.side) ||
                                              !passes(*page, doc_vision::Gate::Light))))) ||
        at - search.startedAt >= kSearchLimit) {
        abortSearch(frame);
        return;
    }
    if (frame.number < search.waitFrom) {
        return; // offered before the lens was told to move
    }
    if (!search.firstCountingAt) {
        search.firstCountingAt = at;
    }
    if (at < *search.firstCountingAt + search.settle) {
        return;
    }
    if (pageSearch) {
        if (!page) {
            return;
        }
        search.samples.push_back(page->sharpness);
    } else {
        search.samples.push_back(clickValue(frame, search.region));
    }
    if (search.samples.size() < static_cast<std::size_t>(kFramesPerStep)) {
        return;
    }
    double sum = 0.0;
    for (const double sample : search.samples) {
        sum += sample;
    }
    m_curve.push_back({.position = search.lensFrom,
                       .sharpness = sum / static_cast<double>(search.samples.size()),
                       .countingFrom = search.waitFrom});
    afterStep(frame);
}

void FocusController::afterStep(const frame_io::Frame& frame) {
    Search& search = *m_search;
    const FocusStep& step = m_curve.back();
    if (step.sharpness > search.best) {
        search.best = step.sharpness;
        search.bestPosition = step.position;
        search.below = 0;
    } else if (step.sharpness < kEarlyStopShare * search.best) {
        ++search.below;
    } else {
        search.below = 0;
    }
    const bool stop = search.stage == Stage::Coarse && search.below >= kEarlyStopSteps && succeeded(search);
    if (!stop && ++search.index < search.positions.size()) {
        moveTo(search.positions[search.index], frame);
        return;
    }
    if (search.stage == Stage::Fine) {
        finishSearch(frame);
        return;
    }
    if (search.stage == Stage::Local && !succeeded(search)) {
        // Nothing sharp near the held position: the full search it falls back to counts as the same search.
        beginStage(Stage::Coarse, coarsePositions(search.lensFrom), frame);
        return;
    }
    const int fine = fineStep();
    std::vector<int> around;
    if (fine > 0) {
        for (const int position : {search.bestPosition - fine, search.bestPosition + fine}) {
            const bool measured = std::any_of(m_curve.begin(), m_curve.end(), [position](const FocusStep& s) {
                return s.position == position;
            });
            if (!measured && position >= m_range->minimum && position <= top()) {
                around.push_back(position);
            }
        }
    }
    beginStage(Stage::Fine, std::move(around), frame);
}

bool FocusController::succeeded(const Search& search) const {
    if (search.kind == FocusStatus::Kind::Page) {
        return search.best >= m_passesAt;
    }
    if (m_curve.empty()) {
        return false;
    }
    std::vector<double> values;
    for (const FocusStep& step : m_curve) {
        values.push_back(step.sharpness);
    }
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2),
                     values.end());
    const double median = values[values.size() / 2];
    return search.best >= kClickFactor * median && search.best >= kClickFloor;
}

void FocusController::finishSearch(const frame_io::Frame& frame) {
    const Search search = *m_search;
    m_search.reset();
    m_lastSearchEnd = frame.at;
    m_status.countingFrom = 0;
    if (succeeded(search)) {
        hold(search, frame);
        return;
    }
    restore();
    m_status.position.reset();
    if (search.kind == FocusStatus::Kind::Page) {
        m_status.state = FocusStatus::State::CannotFocus;
        m_status.afterSearch = true;
        m_cannotSide = m_lastSide;
        if (m_memory) {
            m_memory->blocked = true;
            m_memory->failed = true;
            m_memory->side = m_lastSide;
        }
        setNote("Could not focus on the document");
    } else {
        m_status.state = FocusStatus::State::Camera;
        setNote("Could not focus there");
    }
}

void FocusController::abortSearch(const frame_io::Frame& frame) {
    const Search search = *m_search;
    m_search.reset();
    m_lastSearchEnd = frame.at;
    m_status.countingFrom = 0;
    if (succeeded(search)) {
        hold(search, frame);
        return;
    }
    restore();
    m_status.position.reset();
    m_status.state = FocusStatus::State::Camera;
    if (search.kind == FocusStatus::Kind::Page) {
        if (m_memory) {
            m_memory->blocked = true;
        }
    } else {
        setNote("Could not focus there");
    }
}

void FocusController::hold(const Search& search, const frame_io::Frame& frame) {
    if (search.lensFrom != search.bestPosition) {
        m_search = search; // moveTo works on the search
        const bool moved = moveTo(search.bestPosition, frame);
        m_search.reset();
        if (!moved) {
            return;
        }
    }
    m_status.state = FocusStatus::State::Holding;
    m_status.kind = search.kind;
    m_status.position = search.bestPosition;
    m_status.countingFrom = 0;
    m_heldSide = search.side; // the search's page: one that changed size since lets go at once
    m_heldSince = frame.at;
    if (search.kind == FocusStatus::Kind::Page) {
        m_predictions.emplace_back(search.side, search.bestPosition);
        if (m_memory) {
            m_memory->side = m_lastSide;
        }
        setNote("Focused at " + std::to_string(search.bestPosition) + " in " +
                oneDecimal(seconds(frame.at - search.startedAt)) + " s");
    } else {
        setNote("Focused at " + std::to_string(search.bestPosition) + ": " + wholeNumber(search.before) +
                " → " + wholeNumber(search.best));
    }
}

void FocusController::letGo() {
    restore();
    m_status.state = FocusStatus::State::Camera;
    m_status.position.reset();
}

void FocusController::giveUp() {
    m_reachable = false;
    m_search.reset();
    restore();
    m_status.state = FocusStatus::State::Camera;
    m_status.position.reset();
    m_status.countingFrom = 0;
    setNote("the camera's focus could not be set");
}

// Puts back what the lab found: autofocus on if it was on, else its position. False when that failed.
bool FocusController::restore() {
    if (!m_changed || !m_control || !m_found) {
        return true;
    }
    m_changed = false;
    bool ok = true;
    if (m_found->autoFocus) {
        ok = m_control->setAutoFocus(true);
        m_autoOff = false;
    } else if (m_found->position) {
        ok = m_control->setPosition(*m_found->position);
    }
    if (!ok) {
        m_status.restoreFailed = true;
        m_reachable = false;
    }
    return ok;
}

void FocusController::setNote(std::string note) {
    m_status.note = std::move(note);
    ++m_status.noteNumber;
}

std::optional<int> FocusController::predicted(double side) const {
    std::optional<int> best;
    double closest = kSizeChange;
    for (const auto& [seenSide, position] : m_predictions) {
        const double change = std::abs(side - seenSide) / seenSide;
        if (change <= closest) {
            closest = change;
            best = position;
        }
    }
    return best;
}

// The coarse positions, from the end nearer `start` to the other.
std::vector<int> FocusController::coarsePositions(int start) const {
    std::vector<int> positions;
    const int step = coarseStep();
    const int minimum = m_range->minimum;
    const int maximum = top();
    if (start - minimum <= maximum - start) {
        for (int position = minimum; position <= maximum; position += step) {
            positions.push_back(position);
        }
    } else {
        for (int position = maximum; position >= minimum; position -= step) {
            positions.push_back(position);
        }
    }
    return positions;
}

// The largest position on the camera's grid not above its maximum.
int FocusController::top() const {
    return m_range->minimum + (m_range->maximum - m_range->minimum) / m_range->step * m_range->step;
}

int FocusController::onGrid(int position) const {
    const int clamped = std::clamp(position, m_range->minimum, top());
    const int steps =
        static_cast<int>(std::lround(static_cast<double>(clamped - m_range->minimum) / m_range->step));
    return std::min(m_range->minimum + steps * m_range->step, top());
}

int FocusController::coarseStep() const {
    const int step = m_range->step;
    const double raw =
        std::round(static_cast<double>(m_range->maximum - m_range->minimum) / kCoarseDivisions);
    const int coarse = static_cast<int>(std::lround(raw / step)) * step;
    return std::max(coarse, step);
}

// Half the coarse step on the grid, at least one grid step; 0 when that is the coarse step itself.
int FocusController::fineStep() const {
    const int step = m_range->step;
    const int coarse = coarseStep();
    const int fine = std::max(step, static_cast<int>(std::lround(coarse / 2.0 / step)) * step);
    return fine < coarse ? fine : 0;
}

} // namespace lab_core
