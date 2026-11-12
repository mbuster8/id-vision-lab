#include "doc_vision/guidance.h"

namespace doc_vision {

namespace {

constexpr std::size_t index(Gate gate) { return static_cast<std::size_t>(gate); }

} // namespace

Tone toneOf(Hint hint) {
    // Every hint named, no default: a new one must be given its tone (-Wswitch).
    switch (hint) {
    case Hint::NoPage:
        return Tone::Neutral;
    case Hint::Good:
        return Tone::Good;
    case Hint::CutByEdge:
    case Hint::MoveToMiddle:
    case Hint::TooFar:
    case Hint::TooClose:
    case Hint::Tilted:
    case Hint::Blurred:
    case Hint::Glare:
    case Hint::TooDark:
    case Hint::TooBright:
    case Hint::NoMrzBand:
    case Hint::CannotReadMrz:
        break;
    }
    return Tone::Bad;
}

bool gatesPass(Hint hint) {
    return hint == Hint::Good || hint == Hint::NoMrzBand || hint == Hint::CannotReadMrz;
}

std::optional<Gate> gateOf(Hint hint) {
    switch (hint) {
    case Hint::CutByEdge:
    case Hint::MoveToMiddle:
        return Gate::InView;
    case Hint::TooFar:
    case Hint::TooClose:
        return Gate::Size;
    case Hint::Tilted:
        return Gate::Flat;
    case Hint::Blurred:
        return Gate::Sharp;
    case Hint::Glare:
        return Gate::Glare;
    case Hint::TooDark:
    case Hint::TooBright:
        return Gate::Light;
    case Hint::NoPage:
    case Hint::Good:
    case Hint::NoMrzBand:
    case Hint::CannotReadMrz:
        return std::nullopt;
    }
    return std::nullopt;
}

Hint hintFor(const std::optional<Verdicts>& verdicts) {
    if (!verdicts) {
        return Hint::NoPage;
    }
    const Verdicts& v = *verdicts;
    if (!v.passed[index(Gate::InView)]) {
        return v.cutButWouldFit ? Hint::MoveToMiddle : Hint::CutByEdge;
    }
    if (!v.passed[index(Gate::Size)]) {
        return v.tooFar ? Hint::TooFar : Hint::TooClose;
    }
    if (!v.passed[index(Gate::Flat)]) {
        return Hint::Tilted;
    }
    if (!v.passed[index(Gate::Sharp)]) {
        return Hint::Blurred;
    }
    if (!v.passed[index(Gate::Glare)]) {
        return Hint::Glare;
    }
    if (!v.passed[index(Gate::Light)]) {
        return v.tooDark ? Hint::TooDark : Hint::TooBright;
    }
    return Hint::Good;
}

Hint HintTimer::next(Hint frameHint, std::chrono::nanoseconds at) {
    if (!m_started) {
        m_started = true;
        m_shown = m_candidate = frameHint;
        m_shownSince = m_candidateSince = at;
        return m_shown;
    }
    if (frameHint != m_candidate) {
        m_candidate = frameHint;
        m_candidateSince = at;
    }
    if (m_candidate == m_shown) {
        return m_shown;
    }
    const std::chrono::nanoseconds need =
        m_candidate == Hint::Good ? kGoodAfter : (m_shown == Hint::Good ? kLeaveGoodAfter : kShowAfter);
    const std::chrono::nanoseconds stay = m_shown == Hint::Good ? std::chrono::nanoseconds{0} : kStayAtLeast;
    if (at - m_candidateSince >= need && at - m_shownSince >= stay) {
        m_shown = m_candidate;
        m_shownSince = at;
    }
    return m_shown;
}

void HintTimer::reset() { *this = HintTimer{}; }

ChipStates ChipTimer::next(const std::optional<Verdicts>& verdicts, Hint shown, std::chrono::nanoseconds at) {
    ChipStates states;
    states.visible = shown != Hint::NoPage;
    const std::optional<Gate> pinned = gateOf(shown);
    for (std::size_t i = 0; i < kGateCount; ++i) {
        Chip& chip = m_chips[i];
        if (verdicts) {
            const bool passed = verdicts->passed[i];
            if (!chip.started) {
                chip.started = true;
                chip.shown = chip.candidate = passed;
                chip.candidateSince = at;
            } else if (passed != chip.candidate) {
                chip.candidate = passed;
                chip.candidateSince = at;
            }
            if (chip.candidate != chip.shown && at - chip.candidateSince >= kShowAfter) {
                chip.shown = chip.candidate;
            }
        }
        if (shown == Hint::Good) {
            states.passed[i] = true;
        } else if (pinned && index(*pinned) == i) {
            states.passed[i] = false;
        } else {
            states.passed[i] = chip.shown;
        }
    }
    return states;
}

void ChipTimer::reset() { m_chips = {}; }

std::optional<Held> PageHold::next(const std::optional<std::pair<Quad, Verdicts>>& found,
                                   std::chrono::nanoseconds at) {
    if (found) {
        m_last = Held{found->first, found->second, true};
        m_lastAt = at;
        return m_last;
    }
    if (m_last && at - m_lastAt <= kHoldFor) {
        Held held = *m_last;
        held.fresh = false;
        return held;
    }
    m_last.reset();
    return std::nullopt;
}

void PageHold::reset() { m_last.reset(); }

} // namespace doc_vision
