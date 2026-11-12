#pragma once

#include <optional>

namespace document_lab {

// The scan's states (A4, spec §3.1), without widgets. Guiding covers Reading too: the banner shows which.
// TurnOver (A4.1): a TD1 card's back is accepted, and its front is looked for.
enum class ScanState { Guiding, TurnOver, Scanned, Review };

// Accepted: a result carries a new scan; AcceptedTd1: a new TD1 card's scan, whose front comes next (A4.1).
// FrontKept: a result carries a new front. SkipFront (TurnOver), SkipChip, ScanAgain (TurnOver, Scanned),
// Discard and ScanAnother (Review): the user's buttons and keys. SourceChanged: another source opened, or a
// camera started, while TurnOver shows (it carries the step on, D11). RunEnded: Stop, another camera or
// source (outside TurnOver), another processor.
enum class ScanEvent {
    Accepted,
    AcceptedTd1,
    FrontKept,
    SkipFront,
    SourceChanged,
    SkipChip,
    ScanAgain,
    Discard,
    ScanAnother,
    RunEnded
};

enum class ScanKey { Enter, Escape };

// A4.1: a TD1 card's two sides, its back (the MRZ) and its front (the photo).
enum class CardSide { Back, Front };

// What feeds the engine while the next state shows: everything goes on (None); a playing replay pauses, a
// camera's frames still handed on (Replay: the user turns the card in front of it); a camera's frames no
// longer handed on too (All, as A4's accept).
enum class Pause { None, Replay, All };

struct ScanStep {
    ScanState to = ScanState::Guiding;
    bool resume = false;  // reset the processor and let frames come again
    bool keepScan = true; // false: the scan and the front are dropped
    Pause pause = Pause::None;
    std::optional<bool> frontStage{}; // the processor's front stage on or off, when set
    bool operator==(const ScanStep&) const = default;
};

// Where `event` leads from `state`; nothing when it does not apply there.
std::optional<ScanStep> next(ScanState state, ScanEvent event);

// What a key means in `state` (ADR-004 item 20): in TurnOver, Enter skips the front; in Scanned, it skips the
// chip; in both, Esc scans again. Review's Enter is Save, which the window handles; Review's Esc does
// nothing.
std::optional<ScanEvent> eventFor(ScanState state, ScanKey key);

} // namespace document_lab
