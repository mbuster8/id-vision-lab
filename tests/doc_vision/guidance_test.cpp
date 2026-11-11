#include "doc_vision/guidance.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <random>
#include <utility>

namespace {

using doc_vision::Gate;
using doc_vision::Hint;
using doc_vision::Verdicts;
using std::chrono::milliseconds;

constexpr std::size_t at(Gate gate) { return static_cast<std::size_t>(gate); }

Verdicts allPass() {
    Verdicts v;
    v.passed.fill(true);
    return v;
}

Verdicts failing(Gate gate) {
    Verdicts v = allPass();
    v.passed[at(gate)] = false;
    return v;
}

const doc_vision::Quad kQuad{{{100, 100}, {500, 100}, {500, 380}, {100, 380}}};

TEST(Guidance, HintOrder) {
    EXPECT_EQ(doc_vision::hintFor(std::nullopt), Hint::NoPage);

    Verdicts cut = failing(Gate::InView);
    EXPECT_EQ(doc_vision::hintFor(cut), Hint::CutByEdge);
    cut.cutButWouldFit = true;
    EXPECT_EQ(doc_vision::hintFor(cut), Hint::MoveToMiddle);

    Verdicts far = failing(Gate::Size);
    far.tooFar = true;
    EXPECT_EQ(doc_vision::hintFor(far), Hint::TooFar);
    Verdicts close = failing(Gate::Size);
    close.tooClose = true;
    EXPECT_EQ(doc_vision::hintFor(close), Hint::TooClose);

    EXPECT_EQ(doc_vision::hintFor(failing(Gate::Flat)), Hint::Tilted);
    EXPECT_EQ(doc_vision::hintFor(failing(Gate::Sharp)), Hint::Blurred);
    EXPECT_EQ(doc_vision::hintFor(failing(Gate::Glare)), Hint::Glare);

    Verdicts dark = failing(Gate::Light);
    dark.tooDark = true;
    EXPECT_EQ(doc_vision::hintFor(dark), Hint::TooDark);
    Verdicts bright = failing(Gate::Light);
    bright.tooBright = true;
    EXPECT_EQ(doc_vision::hintFor(bright), Hint::TooBright);

    EXPECT_EQ(doc_vision::hintFor(allPass()), Hint::Good);

    Verdicts first = failing(Gate::InView);
    first.passed[at(Gate::Sharp)] = false;
    EXPECT_EQ(doc_vision::hintFor(first), Hint::CutByEdge);
}

TEST(Guidance, TonesAndGates) {
    EXPECT_EQ(doc_vision::toneOf(Hint::NoPage), doc_vision::Tone::Neutral);
    EXPECT_EQ(doc_vision::toneOf(Hint::Good), doc_vision::Tone::Good);
    EXPECT_EQ(doc_vision::toneOf(Hint::Blurred), doc_vision::Tone::Bad);
    EXPECT_EQ(doc_vision::gateOf(Hint::MoveToMiddle), Gate::InView);
    EXPECT_EQ(doc_vision::gateOf(Hint::TooClose), Gate::Size);
    EXPECT_EQ(doc_vision::gateOf(Hint::TooBright), Gate::Light);
    EXPECT_FALSE(doc_vision::gateOf(Hint::Good).has_value());
    EXPECT_FALSE(doc_vision::gateOf(Hint::NoPage).has_value());
}

TEST(Guidance, FirstHintShowsAtOnce) {
    doc_vision::HintTimer timer;
    EXPECT_EQ(timer.next(Hint::NoPage, milliseconds(0)), Hint::NoPage);
    timer.reset();
    EXPECT_EQ(timer.next(Hint::Blurred, milliseconds(5000)), Hint::Blurred);
}

TEST(Guidance, GoodWaitsHalfASecondAndTheDwell) {
    doc_vision::HintTimer timer;
    timer.next(Hint::NoPage, milliseconds(0));
    EXPECT_EQ(timer.next(Hint::Good, milliseconds(100)), Hint::NoPage);
    EXPECT_EQ(timer.next(Hint::Good, milliseconds(599)), Hint::NoPage);
    EXPECT_EQ(timer.next(Hint::Good, milliseconds(799)), Hint::NoPage);
    EXPECT_EQ(timer.next(Hint::Good, milliseconds(800)), Hint::Good);
}

TEST(Guidance, GoodLeavesAfterATenth) {
    doc_vision::HintTimer timer;
    timer.next(Hint::NoPage, milliseconds(0));
    timer.next(Hint::Good, milliseconds(100));
    ASSERT_EQ(timer.next(Hint::Good, milliseconds(800)), Hint::Good);
    EXPECT_EQ(timer.next(Hint::Blurred, milliseconds(900)), Hint::Good);
    EXPECT_EQ(timer.next(Hint::Blurred, milliseconds(999)), Hint::Good);
    EXPECT_EQ(timer.next(Hint::Blurred, milliseconds(1000)), Hint::Blurred);
}

TEST(Guidance, OtherHintsWaitAndStay) {
    doc_vision::HintTimer timer;
    timer.next(Hint::NoPage, milliseconds(0));
    timer.next(Hint::Good, milliseconds(100));
    timer.next(Hint::Good, milliseconds(800));
    timer.next(Hint::Blurred, milliseconds(900));
    ASSERT_EQ(timer.next(Hint::Blurred, milliseconds(1000)), Hint::Blurred);
    EXPECT_EQ(timer.next(Hint::Glare, milliseconds(1100)), Hint::Blurred);
    EXPECT_EQ(timer.next(Hint::Glare, milliseconds(1799)), Hint::Blurred);
    EXPECT_EQ(timer.next(Hint::Glare, milliseconds(1800)), Hint::Glare);
}

TEST(Guidance, FlickerNeverShows) {
    doc_vision::HintTimer timer;
    timer.next(Hint::NoPage, milliseconds(0));
    for (int t = 100; t <= 3000; t += 100) {
        EXPECT_EQ(timer.next((t / 100) % 2 == 0 ? Hint::Blurred : Hint::Glare, milliseconds(t)), Hint::NoPage)
            << t;
    }
}

TEST(Guidance, GapKeepsTheLastVerdicts) {
    doc_vision::PageHold hold;
    const auto found = hold.next(std::pair{kQuad, allPass()}, milliseconds(0));
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->fresh);
    const auto held = hold.next(std::nullopt, milliseconds(200));
    ASSERT_TRUE(held.has_value());
    EXPECT_FALSE(held->fresh);
    EXPECT_EQ(held->quad, kQuad);
    EXPECT_FALSE(hold.next(std::nullopt, milliseconds(334)).has_value());
}

// Frames 33 ms apart through the hold, the hint and the timer, as the processor runs them.
struct Pipeline {
    doc_vision::PageHold hold;
    doc_vision::HintTimer timer;
    Hint step(bool found, int frame) {
        const auto at = milliseconds(33 * frame);
        const auto held = hold.next(found ? std::optional(std::pair{kQuad, allPass()}) : std::nullopt, at);
        return timer.next(doc_vision::hintFor(held ? std::optional(held->verdicts) : std::nullopt), at);
    }
};

TEST(Guidance, OneMissInFiveKeepsTheHint) {
    Pipeline pipeline;
    ASSERT_EQ(pipeline.step(true, 0), Hint::Good);
    for (int frame = 1; frame < 90; ++frame) {
        EXPECT_EQ(pipeline.step(frame % 5 != 0, frame), Hint::Good) << frame;
    }
}

TEST(Guidance, GoodReachedWithTenPercentMisses) {
    Pipeline pipeline;
    ASSERT_EQ(pipeline.step(false, 0), Hint::NoPage);
    std::mt19937 gen(7);
    Hint shown = Hint::NoPage;
    for (int frame = 1; frame * 33 <= 1000; ++frame) {
        shown = pipeline.step(gen() % 10 != 0, frame);
    }
    EXPECT_EQ(shown, Hint::Good);
}

TEST(Guidance, ChipsAgreeWithTheBanner) {
    doc_vision::ChipTimer chips;
    const doc_vision::ChipStates blurred = chips.next(allPass(), Hint::Blurred, milliseconds(0));
    EXPECT_TRUE(blurred.visible);
    EXPECT_FALSE(blurred.passed[at(Gate::Sharp)]);
    EXPECT_TRUE(blurred.passed[at(Gate::InView)]);

    const doc_vision::ChipStates good = chips.next(failing(Gate::Glare), Hint::Good, milliseconds(33));
    for (const bool passed : good.passed) {
        EXPECT_TRUE(passed);
    }

    EXPECT_FALSE(chips.next(std::nullopt, Hint::NoPage, milliseconds(66)).visible);
    // A frame without verdicts while a hint still shows (the hold, or the banner not yet changed): still
    // visible.
    EXPECT_TRUE(chips.next(std::nullopt, Hint::Blurred, milliseconds(99)).visible);
}

TEST(Guidance, ChipFollowsItsGateAfterThreeTenths) {
    doc_vision::ChipTimer chips;
    EXPECT_TRUE(chips.next(allPass(), Hint::Blurred, milliseconds(0)).passed[at(Gate::Glare)]);
    EXPECT_TRUE(chips.next(failing(Gate::Glare), Hint::Blurred, milliseconds(100)).passed[at(Gate::Glare)]);
    EXPECT_TRUE(chips.next(failing(Gate::Glare), Hint::Blurred, milliseconds(399)).passed[at(Gate::Glare)]);
    EXPECT_FALSE(chips.next(failing(Gate::Glare), Hint::Blurred, milliseconds(400)).passed[at(Gate::Glare)]);
}

} // namespace
