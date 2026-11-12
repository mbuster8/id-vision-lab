#include "lab_core/document_processor.h"
#include "lab_core/frame_processor.h"

#include "fake_mrz_reader.h"
#include "procedural_page.h"
#include "reading_fixture.h"
#include "scripted_finder.h"

#include <doc_vision/guidance.h>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// A4.1, spec §6.4: the Document processor's front stage. After a TD1 card's back is accepted, it reads
// nothing, waits for the card to be turned (its page lost for longer than PageHold bridges, or a reset), then
// keeps the first frame of the front whose banner says Good and whose own page passes every gate.

namespace {

using doc_vision::Hint;
using lab_core::FrontStage;
using lab_core::ProcessorResult;
using test_support::frameAt;
using test_support::goodFrame;
using test_support::nothing;
using test_support::page;
using test_support::Reading;

const lab_core::Finding* findingNamed(const ProcessorResult& result, const std::string& name) {
    for (const lab_core::Finding& finding : result.findings) {
        if (finding.name == name) {
            return &finding;
        }
    }
    return nullptr;
}

std::optional<FrontStage> stageOf(const ProcessorResult& result) {
    return result.guidance ? result.guidance->frontStage : std::nullopt;
}

bool hasFront(const ProcessorResult& result) { return result.guidance && result.guidance->front; }

// A TD1 card's back accepted on frames 0 to 3, then the front stage on: the next frame is 4.
struct BackAccepted {
    Reading lab;
    int next = 0;

    BackAccepted() {
        lab.reader->last = test_support::readingOf(test_support::kTd1Specimen);
        for (; next < 4; ++next) {
            lab.step(frameAt(goodFrame(), next));
        }
        lab.processor->setFrontStage(true);
    }
    ProcessorResult step() { return lab.step(frameAt(goodFrame(), next++)); }
    // From the next frame on, the finder finds nothing for `missed` frames, then the page again.
    void loseThePageFor(int missed) {
        lab.finder->steps.assign(static_cast<std::size_t>(missed), nothing());
        lab.finder->steps.push_back(page(test_support::kPageQuad));
        lab.finder->calls = 0;
    }
};

TEST(FrontStage, ReadsNothingAndShowsThePagesFindings) {
    BackAccepted card;
    const int reads = card.lab.reader->reads;
    ProcessorResult result;
    for (int n = 0; n < 10; ++n) {
        result = card.step();
    }
    EXPECT_EQ(card.lab.reader->reads, reads);
    ASSERT_TRUE(result.guidance);
    EXPECT_FALSE(result.guidance->reading);
    EXPECT_TRUE(findingNamed(result, "Fill"));
    EXPECT_FALSE(findingNamed(result, "MRZ"));
    EXPECT_EQ(stageOf(result), FrontStage::AwaitingTurn);
}

// One missed detection is bridged by PageHold, as always: it is not a turn.
TEST(FrontStage, OneMissedDetectionIsNotATurn) {
    BackAccepted card;
    card.loseThePageFor(1);
    ProcessorResult result;
    for (int n = 0; n < 60; ++n) {
        result = card.step();
        EXPECT_FALSE(hasFront(result)) << n;
    }
    EXPECT_EQ(stageOf(result), FrontStage::AwaitingTurn);
}

// A card turned without the detector losing it: no front is kept, ever (the banner asks for it out of view).
TEST(FrontStage, PageNeverLostKeepsNoFront) {
    BackAccepted card;
    ProcessorResult result;
    for (int n = 0; n < 150; ++n) {
        result = card.step();
        EXPECT_FALSE(hasFront(result)) << n;
    }
    EXPECT_EQ(stageOf(result), FrontStage::AwaitingTurn);
}

TEST(FrontStage, KeepsTheFirstSteadyGoodFrameAfterALoss) {
    BackAccepted card;
    card.loseThePageFor(12); // 396 ms: more than PageHold's third of a second
    bool looking = false;
    std::optional<ProcessorResult> kept;
    for (int n = 0; n < 120 && !kept; ++n) {
        const int number = card.next;
        ProcessorResult result = card.step();
        looking = looking || stageOf(result) == FrontStage::Looking;
        if (hasFront(result)) {
            EXPECT_EQ(result.guidance->front->frame.number, static_cast<std::uint64_t>(number));
            kept = std::move(result);
        }
    }
    EXPECT_TRUE(looking);
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->guidance->shownHint, Hint::Good);
    ASSERT_TRUE(kept->guidance->page);
    EXPECT_TRUE(kept->guidance->page->verdicts.allPassed());
    std::vector<std::string> names;
    for (const lab_core::Finding& gate : kept->guidance->front->gates) {
        names.push_back(gate.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Fill", "Tilt", "Sharpness", "Glare", "Exposure"}));
    // Carried on every result after it.
    const ProcessorResult after = card.step();
    ASSERT_TRUE(hasFront(after));
    EXPECT_EQ(after.guidance->front, kept->guidance->front);
}

// A reset (a new source, a seek, a loop) forgets the frames before it, which counts as the turn; it keeps the
// stage, and drops a kept front.
TEST(FrontStage, ResetCountsAsTheTurn) {
    BackAccepted card;
    card.lab.processor->reset();
    EXPECT_EQ(stageOf(card.step()), FrontStage::Looking);
    bool kept = false;
    for (int n = 0; n < 120 && !kept; ++n) {
        kept = hasFront(card.step());
    }
    ASSERT_TRUE(kept);
    card.loseThePageFor(1); // so the frame after the reset cannot be kept as a new front at once
    card.lab.processor->reset();
    const ProcessorResult after = card.step();
    EXPECT_FALSE(hasFront(after));
    EXPECT_EQ(stageOf(after), FrontStage::Looking);
}

TEST(FrontStage, OffDropsTheFront) {
    BackAccepted card;
    card.lab.processor->reset();
    bool kept = false;
    for (int n = 0; n < 120 && !kept; ++n) {
        kept = hasFront(card.step());
    }
    ASSERT_TRUE(kept);
    card.lab.processor->setFrontStage(false);
    const ProcessorResult after = card.step();
    EXPECT_FALSE(hasFront(after));
    EXPECT_FALSE(stageOf(after));
}

// The stage needs no scan: after a new source in TurnOver the processor holds none (D11).
TEST(FrontStage, WorksWithoutAScan) {
    Reading lab;
    lab.processor->setFrontStage(true);
    ProcessorResult result;
    for (int n = 0; n < 10; ++n) {
        result = lab.step(frameAt(goodFrame(), n));
    }
    EXPECT_EQ(lab.reader->reads, 0);
    ASSERT_TRUE(result.guidance);
    EXPECT_FALSE(result.guidance->reading);
    EXPECT_FALSE(findingNamed(result, "MRZ reads"));
    EXPECT_EQ(stageOf(result), FrontStage::AwaitingTurn);
}

} // namespace
