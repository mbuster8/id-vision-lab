#include "lab_core/focus_controller.h"

#include <doc_vision/guidance.h>
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

bool FocusController::end() noexcept { return true; }

void FocusController::step(const frame_io::Frame& frame, const std::optional<PageFacts>& page) {
    checkControl();
    watch(frame, page);
    const nanoseconds at = frame.at;
    if (m_request) {
        m_request.reset();
        if (!canFocus()) {
            setNote("This camera's focus can't be set from the lab");
        }
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
        if (!canFocus() && steady(at) && m_blurredSince && at - *m_blurredSince >= kCannotFocusAfter) {
            m_status.state = FocusStatus::State::CannotFocus;
            m_status.afterSearch = false;
            m_cannotSide = m_lastSide;
        }
        break;
    case FocusStatus::State::Searching:
    case FocusStatus::State::Holding:
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

bool FocusController::steady(nanoseconds) const {
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

void FocusController::setNote(std::string note) {
    m_status.note = std::move(note);
    ++m_status.noteNumber;
}

} // namespace lab_core
