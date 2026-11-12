#include "scan_flow.h"

#include <gtest/gtest.h>

#include <optional>

// A4's scan: Guiding (and Reading, which the banner shows) → Scanned → Review (spec §3.1).

namespace {

using document_lab::eventFor;
using document_lab::next;
using document_lab::ScanEvent;
using document_lab::ScanKey;
using document_lab::ScanState;
using document_lab::ScanStep;

struct Case {
    ScanState from;
    ScanEvent event;
    std::optional<ScanStep> step;
};

TEST(ScanFlow, EveryTransition) {
    const ScanStep toScanned{.to = ScanState::Scanned, .resume = false, .keepScan = true};
    const ScanStep toReview{.to = ScanState::Review, .resume = false, .keepScan = true};
    const ScanStep backToGuiding{.to = ScanState::Guiding, .resume = true, .keepScan = false};
    const ScanStep runEnded{.to = ScanState::Guiding, .resume = false, .keepScan = false};
    const Case cases[] = {
        // An accept leaves Guiding (whatever the banner shows) for Scanned; nothing else applies there.
        {ScanState::Guiding, ScanEvent::Accepted, toScanned},
        {ScanState::Guiding, ScanEvent::SkipChip, std::nullopt},
        {ScanState::Guiding, ScanEvent::ScanAgain, std::nullopt},
        {ScanState::Guiding, ScanEvent::Discard, std::nullopt},
        {ScanState::Guiding, ScanEvent::ScanAnother, std::nullopt},
        {ScanState::Guiding, ScanEvent::RunEnded, std::nullopt},
        // Scanned: Skip the chip, Scan again, or the run ends.
        {ScanState::Scanned, ScanEvent::Accepted, std::nullopt},
        {ScanState::Scanned, ScanEvent::SkipChip, toReview},
        {ScanState::Scanned, ScanEvent::ScanAgain, backToGuiding},
        {ScanState::Scanned, ScanEvent::Discard, std::nullopt},
        {ScanState::Scanned, ScanEvent::ScanAnother, std::nullopt},
        {ScanState::Scanned, ScanEvent::RunEnded, runEnded},
        // Review: Discard, Scan another, or the run ends.
        {ScanState::Review, ScanEvent::Accepted, std::nullopt},
        {ScanState::Review, ScanEvent::SkipChip, std::nullopt},
        {ScanState::Review, ScanEvent::ScanAgain, std::nullopt},
        {ScanState::Review, ScanEvent::Discard, backToGuiding},
        {ScanState::Review, ScanEvent::ScanAnother, backToGuiding},
        {ScanState::Review, ScanEvent::RunEnded, runEnded},
    };
    for (const Case& c : cases) {
        const std::optional<ScanStep> step = next(c.from, c.event);
        EXPECT_EQ(step, c.step) << static_cast<int>(c.from) << " " << static_cast<int>(c.event);
    }
}

// ADR-004 item 20: Esc goes back one step, Enter takes the screen's main action. Review's Enter is Save,
// which the window handles; Review's Esc does nothing (the spec's call 5).
TEST(ScanFlow, Keys) {
    EXPECT_EQ(eventFor(ScanState::Scanned, ScanKey::Enter), ScanEvent::SkipChip);
    EXPECT_EQ(eventFor(ScanState::Scanned, ScanKey::Escape), ScanEvent::ScanAgain);
    EXPECT_EQ(eventFor(ScanState::Review, ScanKey::Enter), std::nullopt);
    EXPECT_EQ(eventFor(ScanState::Review, ScanKey::Escape), std::nullopt);
    EXPECT_EQ(eventFor(ScanState::Guiding, ScanKey::Enter), std::nullopt);
    EXPECT_EQ(eventFor(ScanState::Guiding, ScanKey::Escape), std::nullopt);
}

} // namespace
