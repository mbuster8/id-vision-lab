#pragma once

#include <optional>

namespace document_lab {

// The scan's states (A4, spec §3.1), without widgets. Guiding covers Reading too: the banner shows which.
enum class ScanState { Guiding, Scanned, Review };

// Accepted: a result carries a new scan. SkipChip, ScanAgain (Scanned), Discard and ScanAnother (Review): the
// user's buttons and keys. RunEnded: Stop, another camera or source, another processor.
enum class ScanEvent { Accepted, SkipChip, ScanAgain, Discard, ScanAnother, RunEnded };

enum class ScanKey { Enter, Escape };

struct ScanStep {
    ScanState to = ScanState::Guiding;
    bool resume = false;  // reset the processor and let frames come again
    bool keepScan = true; // false: the scan is dropped
    bool operator==(const ScanStep&) const = default;
};

// Where `event` leads from `state`; nothing when it does not apply there.
std::optional<ScanStep> next(ScanState state, ScanEvent event);

// What a key means in `state` (ADR-004 item 20): in Scanned, Enter skips the chip and Esc scans again.
// Review's Enter is Save, which the window handles; Review's Esc does nothing.
std::optional<ScanEvent> eventFor(ScanState state, ScanKey key);

} // namespace document_lab
