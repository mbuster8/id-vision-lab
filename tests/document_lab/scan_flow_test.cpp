#include "scan_flow.h"

#include <gtest/gtest.h>

#include <optional>

// A4's scan: Guiding (and Reading, which the banner shows) → Scanned → Review (spec §3.1). A4.1: a TD1 card's
// back goes to TurnOver first, for its front (spec §6.5's table).

namespace {

using document_lab::eventFor;
using document_lab::next;
using document_lab::Pause;
using document_lab::ScanEvent;
using document_lab::ScanKey;
using document_lab::ScanState;
using document_lab::ScanStep;

struct Case {
    ScanState from;
    ScanEvent event;
    std::optional<ScanStep> step;
};

constexpr ScanEvent kEvents[] = {ScanEvent::Accepted,  ScanEvent::AcceptedTd1,   ScanEvent::FrontKept,
                                 ScanEvent::SkipFront, ScanEvent::SourceChanged, ScanEvent::SkipChip,
                                 ScanEvent::ScanAgain, ScanEvent::Discard,       ScanEvent::ScanAnother,
                                 ScanEvent::RunEnded};

TEST(ScanFlow, EveryTransition) {
    const ScanStep toScanned{
        .to = ScanState::Scanned, .resume = false, .keepScan = true, .pause = Pause::All, .frontStage = {}};
    const ScanStep toTurnOver{.to = ScanState::TurnOver,
                              .resume = false,
                              .keepScan = true,
                              .pause = Pause::Replay,
                              .frontStage = true};
    const ScanStep stayInTurnOver{
        .to = ScanState::TurnOver, .resume = false, .keepScan = true, .pause = Pause::None, .frontStage = {}};
    const ScanStep toReview{
        .to = ScanState::Review, .resume = false, .keepScan = true, .pause = Pause::None, .frontStage = {}};
    const ScanStep backToGuiding{.to = ScanState::Guiding,
                                 .resume = true,
                                 .keepScan = false,
                                 .pause = Pause::None,
                                 .frontStage = false};
    const ScanStep runEnded{.to = ScanState::Guiding,
                            .resume = false,
                            .keepScan = false,
                            .pause = Pause::None,
                            .frontStage = false};
    const Case cases[] = {
        // An accept leaves Guiding (whatever the banner shows): a TD1 card's back for TurnOver, the rest for
        // Scanned.
        {ScanState::Guiding, ScanEvent::Accepted, toScanned},
        {ScanState::Guiding, ScanEvent::AcceptedTd1, toTurnOver},
        // TurnOver: the front kept or skipped, another source carrying the step on (D11), Scan again, the
        // end.
        {ScanState::TurnOver, ScanEvent::SourceChanged, stayInTurnOver},
        {ScanState::TurnOver, ScanEvent::FrontKept, toScanned},
        {ScanState::TurnOver, ScanEvent::SkipFront, toScanned},
        {ScanState::TurnOver, ScanEvent::ScanAgain, backToGuiding},
        {ScanState::TurnOver, ScanEvent::RunEnded, runEnded},
        // Scanned: Skip the chip, Scan again, or the run ends (another source there ends it too).
        {ScanState::Scanned, ScanEvent::SkipChip, toReview},
        {ScanState::Scanned, ScanEvent::ScanAgain, backToGuiding},
        {ScanState::Scanned, ScanEvent::RunEnded, runEnded},
        // Review: Discard, Scan another, or the run ends.
        {ScanState::Review, ScanEvent::Discard, backToGuiding},
        {ScanState::Review, ScanEvent::ScanAnother, backToGuiding},
        {ScanState::Review, ScanEvent::RunEnded, runEnded},
    };
    for (const ScanState from :
         {ScanState::Guiding, ScanState::TurnOver, ScanState::Scanned, ScanState::Review}) {
        for (const ScanEvent event : kEvents) {
            std::optional<ScanStep> expected;
            for (const Case& c : cases) {
                if (c.from == from && c.event == event) {
                    expected = c.step;
                }
            }
            EXPECT_EQ(next(from, event), expected)
                << static_cast<int>(from) << " " << static_cast<int>(event);
        }
    }
}

// ADR-004 item 20: Esc goes back one step, Enter takes the screen's main action. Review's Enter is Save,
// which the window handles; Review's Esc does nothing (the spec's call 5). A4.1: TurnOver's Enter skips the
// front, its Esc scans again.
TEST(ScanFlow, Keys) {
    EXPECT_EQ(eventFor(ScanState::TurnOver, ScanKey::Enter), ScanEvent::SkipFront);
    EXPECT_EQ(eventFor(ScanState::TurnOver, ScanKey::Escape), ScanEvent::ScanAgain);
    EXPECT_EQ(eventFor(ScanState::Scanned, ScanKey::Enter), ScanEvent::SkipChip);
    EXPECT_EQ(eventFor(ScanState::Scanned, ScanKey::Escape), ScanEvent::ScanAgain);
    EXPECT_EQ(eventFor(ScanState::Review, ScanKey::Enter), std::nullopt);
    EXPECT_EQ(eventFor(ScanState::Review, ScanKey::Escape), std::nullopt);
    EXPECT_EQ(eventFor(ScanState::Guiding, ScanKey::Enter), std::nullopt);
    EXPECT_EQ(eventFor(ScanState::Guiding, ScanKey::Escape), std::nullopt);
}

} // namespace
