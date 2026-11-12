#include "lab_core/focus_controller.h"

#include "fake_focus_control.h"

#include <doc_vision/guidance.h>
#include <doc_vision/quality_gates.h>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using doc_vision::Gate;
using doc_vision::Hint;
using lab_core::FocusController;
using lab_core::FocusStatus;
using State = FocusStatus::State;

const cv::Size kFrame{1920, 1080};
constexpr std::chrono::nanoseconds kFrameTime = 33ms;

// The C920's sharpness against its focus position, as the spike measured it at about half the frame: sharp
// near 30, about 4 elsewhere (spec §12).
struct Curve {
    double floor = 4.0;
    double peak = 500.0;
    double at = 30.0;
    double width = 6.0;
    double bumpAt = 0.0; // a second, lower peak, where a test wants one
    double bumpPeak = 0.0;
    double operator()(int position) const {
        const double d = (position - at) / width;
        const double b = (position - bumpAt) / width;
        return floor + peak * std::exp(-d * d) + bumpPeak * std::exp(-b * b);
    }
};

// One camera run's frames, with a pretend lens and a pretend page: the lens takes a move only 100 ms + 2 ms
// per unit after the frame that made it (within the settle rule), and the page's sharpness follows the lens.
class CameraRun {
public:
    explicit CameraRun(bool withControl = true) {
        std::unique_ptr<lab_core::FocusControl> control;
        if (withControl) {
            control = std::make_unique<test_support::FakeFocusControl>(log);
        }
        controller = std::make_unique<FocusController>(std::move(control), [this] { return nextOffer(); });
    }

    std::shared_ptr<test_support::FocusLog> log = std::make_shared<test_support::FocusLog>();
    std::unique_ptr<FocusController> controller;
    Curve curve;
    double side = 0.48;       // the page's longer side, as a share of the frame's
    double dx = 0.0;          // the page moved sideways, in pixels
    bool page = true;         // a fresh page in the next frame
    std::optional<Hint> hint; // the frame's hint, when not the lens's
    bool lightFails = false;
    bool numbered = false;        // each frame's sharpness 1000 + its number
    bool sharpFlicker = false;    // the next frame alone passes Sharp, as a noisy one may
    std::uint64_t offerAhead = 1; // nextOffer answers this frame's number plus this: 2 when one frame waits
    bool offerThrows = false;     // nextOffer throws, as a full memory might part-way through a step
    std::chrono::nanoseconds frameTime = kFrameTime; // between processed frames: 30 a second
    std::uint64_t number = 0;
    std::chrono::nanoseconds at{};
    int searches = 0; // transitions into Searching
    FocusStatus last;

    // The lens's position for a frame taken at `t`.
    int lensAt(std::chrono::nanoseconds t) const {
        int position = m_startPosition ? *m_startPosition : log->position;
        for (const test_support::FocusLog::Move& move : log->moves) {
            const auto lag = 100ms + 2ms * std::abs(move.to - move.from);
            if (t >= move.at + lag) {
                position = move.to;
            }
        }
        return position;
    }

    double sharpness(std::chrono::nanoseconds t) const {
        return numbered ? 1000.0 + static_cast<double>(number) : curve(lensAt(t));
    }

    std::optional<lab_core::PageFacts> facts() {
        if (!page) {
            return std::nullopt;
        }
        const double s = sharpFlicker ? 60.0 : sharpness(at);
        // A3.1's band: fails under 50, passes again from 55.
        if (m_sharpPassing && s < 50.0) {
            m_sharpPassing = false;
        } else if (!m_sharpPassing && s >= 55.0) {
            m_sharpPassing = true;
        }
        const double w = side * kFrame.width;
        const double h = w / 1.42;
        const double cx = kFrame.width / 2.0 + dx;
        const double cy = kFrame.height / 2.0;
        lab_core::PageFacts facts;
        facts.quad = {{{cx - w / 2, cy - h / 2},
                       {cx + w / 2, cy - h / 2},
                       {cx + w / 2, cy + h / 2},
                       {cx - w / 2, cy + h / 2}}};
        facts.frameSize = kFrame;
        facts.sharpness = s;
        facts.longerSide = side;
        facts.verdicts.passed.fill(true);
        facts.verdicts.passed[static_cast<std::size_t>(Gate::Sharp)] = m_sharpPassing || lightFails;
        facts.verdicts.passed[static_cast<std::size_t>(Gate::Light)] = !lightFails;
        facts.frameHint =
            hint ? *hint : (lightFails ? Hint::TooDark : (m_sharpPassing ? Hint::Good : Hint::Blurred));
        if (sharpFlicker) {
            sharpFlicker = false;
            m_sharpPassing = false; // only this frame
            facts.verdicts.passed[static_cast<std::size_t>(Gate::Sharp)] = true;
            facts.frameHint = Hint::Good;
        }
        return facts;
    }

    FocusStatus frame() {
        if (!m_startPosition) {
            m_startPosition = log->position;
        }
        log->now = at;
        frame_io::Frame frame{
            .image = picture ? picture(lensAt(at)) : image, .number = number, .at = at, .tags = tags};
        const State before = last.state;
        last = controller->next(frame, facts());
        if (last.state == State::Searching && before != State::Searching) {
            ++searches;
        }
        ++number;
        at += frameTime;
        return last;
    }

    // Frames until `t` has passed since this call.
    FocusStatus framesFor(std::chrono::nanoseconds t) {
        const std::chrono::nanoseconds until = at + t;
        while (at < until) {
            frame();
        }
        return last;
    }

    // Frames until the state is `state`, at most `limit` of them; whether it was reached.
    bool until(State state, std::chrono::nanoseconds limit = 20s) {
        const std::chrono::nanoseconds until = at + limit;
        while (at < until) {
            if (frame().state == state) {
                return true;
            }
        }
        return false;
    }

    cv::Mat image;                            // the frames' picture: empty unless a click test draws one
    std::function<cv::Mat(int lens)> picture; // or one drawn for where the lens is
    visual_debug::ImageDescription tags{};

    // The camera's own autofocus moved the lens to `position` while the lab was not driving it.
    void wander(int position) {
        log->moves.push_back({.at = at - 10s, .from = log->position, .to = position});
        log->position = position;
    }

    // The first position the lab set after `calls` calls had been made; nothing if none.
    std::optional<int> firstMoveAfter(std::size_t calls) const {
        for (std::size_t i = calls; i < log->calls.size(); ++i) {
            if (log->calls[i].rfind("position ", 0) == 0) {
                return std::stoi(log->calls[i].substr(9));
            }
        }
        return std::nullopt;
    }

    bool visited(int position) const { return log->count("position " + std::to_string(position)) > 0; }

    // Frames until the search holds; the state reached.
    State searchToTheEnd(std::chrono::nanoseconds limit = 20s) {
        if (!until(State::Searching, limit)) {
            return last.state;
        }
        const std::chrono::nanoseconds until = at + limit;
        while (at < until && frame().state == State::Searching) {
        }
        return last.state;
    }

private:
    std::uint64_t nextOffer() const {
        if (offerThrows) {
            throw std::bad_alloc();
        }
        return number + offerAhead;
    }
    std::optional<int> m_startPosition;
    bool m_sharpPassing = true;
};

// --- Task 3: no control, and a control without a range ---

TEST(FocusController, NoControlNoPageStaysCamera) {
    CameraRun run(false);
    run.page = false;
    for (int i = 0; i < 10; ++i) {
        run.frame();
    }
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_FALSE(run.last.controllable);
    EXPECT_EQ(lab_core::focusFinding(run.last),
              (lab_core::Finding{"Focus", "camera (lab can't set it)", lab_core::Finding::Tone::Plain}));
}

TEST(FocusController, CannotFocusWithoutAControlAfterThreeSeconds) {
    CameraRun run(false); // the lens stays at 120: about 4, blurred
    run.framesFor(2900ms);
    EXPECT_EQ(run.last.state, State::Camera);
    run.framesFor(200ms);
    EXPECT_EQ(run.last.state, State::CannotFocus);
    EXPECT_FALSE(run.last.afterSearch);
    EXPECT_EQ(lab_core::focusFinding(run.last),
              (lab_core::Finding{"Focus", "can't focus", lab_core::Finding::Tone::Bad}));
}

TEST(FocusController, TooFarStartsNothing) {
    CameraRun run(false);
    run.hint = Hint::TooFar;
    run.framesFor(5s);
    EXPECT_EQ(run.last.state, State::Camera);
}

TEST(FocusController, UnsteadyPageStartsNothing) {
    CameraRun run(false);
    for (int i = 0; i < 150; ++i) {
        run.dx = i % 2 == 0 ? 0.0 : 0.03 * kFrame.width;
        run.frame();
        ASSERT_NE(run.last.state, State::CannotFocus) << i;
    }
}

TEST(FocusController, HeldFramesNeitherBreakNorExtend) {
    CameraRun run(false);
    while (run.at < 3100ms) {
        run.page = run.number % 5 != 4; // every fifth frame: PageHold's, not fresh
        run.frame();
    }
    EXPECT_EQ(run.last.state, State::CannotFocus);
}

TEST(FocusController, CannotFocusLeavesWhenSharp) {
    CameraRun run(false);
    ASSERT_TRUE(run.until(State::CannotFocus));
    run.curve.at = 120; // the page turns sharp where the lens is
    run.frame();
    EXPECT_EQ(run.last.state, State::Camera);
}

TEST(FocusController, CannotFocusLeavesWhenTheSizeChanges) {
    CameraRun run(false);
    ASSERT_TRUE(run.until(State::CannotFocus));
    run.side = 0.48 * 1.2;
    run.frame();
    EXPECT_EQ(run.last.state, State::Camera);
}

TEST(FocusController, CannotFocusLeavesWhenLost) {
    CameraRun run(false);
    ASSERT_TRUE(run.until(State::CannotFocus));
    run.page = false;
    run.framesFor(900ms);
    EXPECT_EQ(run.last.state, State::CannotFocus);
    run.framesFor(200ms);
    EXPECT_EQ(run.last.state, State::Camera);
}

TEST(FocusController, ControlWithoutARangeIsNoControl) {
    CameraRun run;
    run.log->range = std::nullopt;
    run.framesFor(3100ms);
    EXPECT_EQ(run.last.state, State::CannotFocus);
    EXPECT_FALSE(run.last.controllable);
    EXPECT_TRUE(run.last.note.empty());
    EXPECT_EQ(run.log->calls, std::vector<std::string>{"range"});
}

TEST(FocusController, ControlWithoutARangeIgnoresClicks) {
    CameraRun run;
    run.log->range = std::nullopt;
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    run.frame();
    EXPECT_EQ(run.last.note, "This camera's focus can't be set from the lab");
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_EQ(run.log->calls, std::vector<std::string>{"range"});
}

// --- Task 4: the page search ---

TEST(FocusController, SearchFindsThePeakAndHolds) {
    CameraRun run; // the lens at 120 with the camera's autofocus on: about 4, blurred
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_EQ(run.last.position, 30);
    EXPECT_EQ(run.log->position, 30);
    EXPECT_FALSE(run.log->autoFocus);
    EXPECT_EQ(lab_core::focusFinding(run.last),
              (lab_core::Finding{"Focus", "lab, 30", lab_core::Finding::Tone::Plain}));
    EXPECT_EQ(run.last.note.rfind("Focused at 30 in ", 0), 0u) << run.last.note;
    EXPECT_EQ(run.searches, 1);
}

TEST(FocusController, TakeOverWaitsOneAndAHalfSeconds) {
    CameraRun run;
    run.framesFor(1400ms);
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_EQ(run.log->count("auto off"), 0);
    run.framesFor(200ms);
    EXPECT_EQ(run.last.state, State::Searching);
}

TEST(FocusController, TooFarOrTiltedStartsNoSearch) {
    CameraRun run;
    run.hint = Hint::TooFar;
    run.framesFor(5s);
    run.hint = Hint::Tilted;
    run.framesFor(5s);
    EXPECT_EQ(run.searches, 0);
    EXPECT_EQ(run.log->count("auto off"), 0);
}

// Review Focus 1: a frame offered before the lens was told to move never counts for the new position.
TEST(FocusController, FrameOfferedBeforeTheMoveNeverCounts) {
    CameraRun run;
    run.offerAhead = 2; // one frame always waiting when the lab moves the lens
    ASSERT_TRUE(run.until(State::Searching));
    const std::uint64_t k = run.number - 1; // the frame that made the first move: 0 → from 120, 120 units
    run.numbered = true;
    while (run.controller->curve().empty() && run.at < 20s) {
        run.frame();
    }
    ASSERT_FALSE(run.controller->curve().empty());
    EXPECT_EQ(run.controller->curve().front().countingFrom, k + 2);
    // From frame k + 2 (at_k + 66 ms), 150 + 2 × 110 = 370 ms: frames k + 14 and k + 15.
    EXPECT_DOUBLE_EQ(run.controller->curve().front().sharpness, 1000.0 + static_cast<double>(k) + 14.5);
}

TEST(FocusController, SettleGrowsWithTheJump) {
    CameraRun run;
    run.log->range = lab_core::FocusRange{.minimum = 0, .maximum = 500, .step = 10};
    run.log->position = 220; // nearer end 0: a move of 220 units, settle 150 + 2 × 210 = 570 ms
    ASSERT_TRUE(run.until(State::Searching));
    const std::uint64_t k = run.number - 1;
    run.numbered = true;
    while (run.controller->curve().empty() && run.at < 20s) {
        run.frame();
    }
    ASSERT_FALSE(run.controller->curve().empty());
    EXPECT_EQ(run.controller->curve().front().position, 0);
    // The first counting frame is k + 1 (at_k + 33); from at_k + 603 ms: frames k + 19 and k + 20.
    EXPECT_DOUBLE_EQ(run.controller->curve().front().sharpness, 1000.0 + static_cast<double>(k) + 19.5);
}

TEST(FocusController, EarlyStopFromTheNearEnd) {
    CameraRun run;
    run.log->position = 0;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_TRUE(run.visited(60));
    EXPECT_FALSE(run.visited(70));
}

TEST(FocusController, EarlyStopIsConsecutive) {
    CameraRun run;
    run.log->position = 0;
    run.curve.bumpAt = 50;
    run.curve.bumpPeak = 200; // 204 at 50: above 30% of the best, so the count of steps below starts again
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_TRUE(run.visited(80));
    EXPECT_FALSE(run.visited(90));
}

TEST(FocusController, FineStepsEitherSideOfTheBest) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    std::vector<int> positions;
    for (const lab_core::FocusStep& step : run.controller->curve()) {
        positions.push_back(step.position);
    }
    EXPECT_NE(std::find(positions.begin(), positions.end(), 25), positions.end());
    EXPECT_NE(std::find(positions.begin(), positions.end(), 35), positions.end());
}

TEST(FocusController, PositionsStayOnTheGrid) {
    CameraRun run;
    run.log->range = lab_core::FocusRange{.minimum = 0, .maximum = 250, .step = 3};
    run.log->position = 240; // nearer the top, which is 249 on this grid
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    for (const std::string& call : run.log->calls) {
        if (call.rfind("position ", 0) == 0 && call != "position?") {
            const int position = std::stoi(call.substr(9));
            EXPECT_EQ(position % 3, 0) << call;
            EXPECT_LE(position, 249) << call;
        }
    }
    EXPECT_TRUE(run.visited(249));
}

TEST(FocusController, NeverSharpGivesCannotFocusAndRestores) {
    CameraRun run;
    run.curve.peak = 36; // never above 40: no position reaches 55
    ASSERT_EQ(run.searchToTheEnd(), State::CannotFocus);
    EXPECT_TRUE(run.last.afterSearch);
    EXPECT_TRUE(run.log->autoFocus);
    EXPECT_EQ(lab_core::focusFinding(run.last),
              (lab_core::Finding{"Focus", "can't focus", lab_core::Finding::Tone::Bad}));
    EXPECT_EQ(run.last.note, "Could not focus on the document"); // a card has no page
}

// Review Focus 4: after a failed search, tremor and a noisy sharp frame start no second sweep.
TEST(FocusController, TremorAfterAFailedSearchStartsNoSecondSweep) {
    CameraRun run;
    run.curve.peak = 36;
    ASSERT_EQ(run.searchToTheEnd(), State::CannotFocus);
    const std::chrono::nanoseconds end = run.at + 10s;
    while (run.at < end) {
        run.sharpFlicker = true; // leaves CannotFocus: only the page memory stops a search now
        run.framesFor(2s);
        for (int i = 0; i < 15; ++i) {
            run.dx = i % 2 == 0 ? 0.03 * kFrame.width : 0.0;
            run.frame();
        }
        run.dx = 0.0;
    }
    EXPECT_EQ(run.searches, 1);
}

TEST(FocusController, TremorWithoutAFailedSearchStartsOne) {
    CameraRun run;
    run.curve.peak = 36;
    const std::chrono::nanoseconds end = run.at + 10s;
    while (run.at < end && run.searches == 0) {
        run.framesFor(2s);
        for (int i = 0; i < 15 && run.searches == 0; ++i) {
            run.dx = i % 2 == 0 ? 0.03 * kFrame.width : 0.0;
            run.frame();
        }
        run.dx = 0.0;
    }
    EXPECT_EQ(run.searches, 1);
}

TEST(FocusController, SizeChangeAllowsANewSearch) {
    CameraRun run;
    run.curve.peak = 36;
    ASSERT_EQ(run.searchToTheEnd(), State::CannotFocus);
    run.framesFor(3s); // the cool-down
    run.side = 0.48 * 1.2;
    EXPECT_TRUE(run.until(State::Searching, 3s));
    EXPECT_EQ(run.searches, 2);
}

TEST(FocusController, LossAllowsANewSearch) {
    CameraRun run;
    run.curve.peak = 36;
    ASSERT_EQ(run.searchToTheEnd(), State::CannotFocus);
    run.page = false;
    run.framesFor(1100ms);
    run.page = true;
    EXPECT_TRUE(run.until(State::Searching, 6s));
    EXPECT_EQ(run.searches, 2);
}

TEST(FocusController, AtMostTwoSearchesPerPage) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.curve.at = 60; // the page now sharp at 60, outside the local search's 10 to 50
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_EQ(run.last.position, 60);
    EXPECT_EQ(run.searches, 2);
    // Final review F4: no third search; the lab gives the focus back to the camera rather than hold a
    // position known not to work.
    run.curve.at = 120;
    run.framesFor(8s);
    EXPECT_EQ(run.searches, 2);
    EXPECT_NE(run.last.state, State::Holding);
    EXPECT_TRUE(run.log->autoFocus);
}

TEST(FocusController, LightFailingAbortsTheSearch) {
    CameraRun run;
    ASSERT_TRUE(run.until(State::Searching));
    while (run.last.position != 10 && run.at < 20s) {
        run.frame();
    }
    run.lightFails = true;
    run.frame();
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_TRUE(run.log->autoFocus);
}

TEST(FocusController, LostPageAbortsAndKeepsAPassingBest) {
    CameraRun run;
    ASSERT_TRUE(run.until(State::Searching));
    const auto measured30 = [&] {
        for (const lab_core::FocusStep& step : run.controller->curve()) {
            if (step.position == 30) {
                return true;
            }
        }
        return false;
    };
    while (!measured30() && run.at < 20s) {
        run.frame();
    }
    run.page = false;
    ASSERT_TRUE(run.until(State::Holding, 2s));
    EXPECT_EQ(run.last.position, 30);
    run.framesFor(1100ms);
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_TRUE(run.log->autoFocus);
}

TEST(FocusController, SearchStopsAtTwelveSeconds) {
    CameraRun run;
    ASSERT_TRUE(run.until(State::Searching));
    run.offerAhead = 1'000'000; // no frame ever counts
    run.framesFor(12s);
    EXPECT_NE(run.last.state, State::Searching);
    EXPECT_TRUE(run.log->autoFocus);
}

TEST(FocusController, SearchStopsAtTwelveSecondsHoldsAPassingBest) {
    CameraRun run;
    ASSERT_TRUE(run.until(State::Searching));
    while (run.controller->curve().size() < 4 && run.at < 20s) { // 0, 10, 20, 30
        run.frame();
    }
    run.offerAhead = 1'000'000;
    run.framesFor(12s);
    EXPECT_EQ(run.last.state, State::Holding);
    EXPECT_EQ(run.last.position, 30);
}

// A3.3's F6: at 8 processed frames a second, with a frame waiting at each move, a step takes about 625 ms, so
// a sweep from the far end to the peak at 30 needs about 15 s. A search that is still measuring steps goes
// on past 12 s; one whose step has had no counting frame for 2 s ends (above).
TEST(FocusController, SlowFramesStillFinishTheSweep) {
    CameraRun run;
    run.frameTime = 125ms;
    run.offerAhead = 2;
    run.log->position = 180; // where the camera's autofocus left the lens: the sweep starts at 250
    ASSERT_EQ(run.searchToTheEnd(40s), State::Holding);
    EXPECT_EQ(run.last.position, 30);
    EXPECT_TRUE(run.visited(250));
}

TEST(FocusController, HoldingLetsGoWhenTheSizeChanges) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.side = 0.48 * 0.8;
    run.frame();
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_TRUE(run.log->autoFocus);
}

// A4: Scanned and Review stop the camera's frames for a while; when they come again A3.3's rules apply
// unchanged (spec §4.2). The passport back under the camera keeps the focus found for it...
TEST(FocusController, HoldSurvivesAGapWithThePageInView) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.at += 30s; // the chip step and Review
    run.frame();
    EXPECT_EQ(run.last.state, State::Holding);
    EXPECT_FALSE(run.log->autoFocus);
}

// ...and frames without it give the camera its own focus back.
TEST(FocusController, HoldLetsGoAfterAGapWithoutThePage) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.at += 30s;
    run.page = false;
    run.frame();
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_TRUE(run.log->autoFocus);
}

TEST(FocusController, HoldingResearchesLocallyFirst) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    const std::size_t calls = run.log->calls.size();
    run.curve.at = 40;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_EQ(run.last.position, 40);
    for (std::size_t i = calls; i < run.log->calls.size(); ++i) {
        const std::string& call = run.log->calls[i];
        if (call.rfind("position ", 0) == 0 && call != "position?") {
            const int position = std::stoi(call.substr(9));
            EXPECT_GE(position, 10) << call;
            EXPECT_LE(position, 50) << call;
        }
    }
}

TEST(FocusController, PredictedPositionStartsTheNextSearch) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.page = false;
    run.framesFor(1100ms);
    ASSERT_EQ(run.last.state, State::Camera);
    run.wander(200); // the camera's own autofocus moved the lens: position() alone would start at 250
    run.page = true;
    const std::size_t calls = run.log->calls.size();
    ASSERT_TRUE(run.until(State::Searching, 6s));
    EXPECT_EQ(run.firstMoveAfter(calls), 0);
}

TEST(FocusController, NoPredictionForAnotherSize) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.page = false;
    run.framesFor(1100ms);
    run.wander(200);
    run.page = true;
    run.side = 0.60;
    const std::size_t calls = run.log->calls.size();
    ASSERT_TRUE(run.until(State::Searching, 6s));
    EXPECT_EQ(run.firstMoveAfter(calls), 250);
}

TEST(FocusController, AFailingCallGivesUp) {
    CameraRun run;
    run.log->failSetPosition = true;
    ASSERT_TRUE(run.until(State::Searching, 3s) || run.log->count("auto off") == 1);
    run.frame();
    EXPECT_FALSE(run.last.controllable);
    EXPECT_EQ(run.last.note, "the camera's focus could not be set");
    EXPECT_EQ(run.log->count("auto on"), 1);
    const std::size_t calls = run.log->calls.size();
    run.framesFor(5s);
    EXPECT_EQ(run.log->calls.size(), calls);
}

// A3.3's F5: a throw part-way through a step (memory exhausted) gives up as a failing call does, rather than
// leave a search half-begun with the camera's autofocus off: the camera's focus is put back, the state says
// so, and the lab leaves the focus alone for the rest of the run.
TEST(FocusController, ThrowPartWayGivesUp) {
    CameraRun run;
    run.framesFor(1s); // sharp: the camera's own focus
    ASSERT_EQ(run.last.state, State::Camera);

    run.offerThrows = true;
    run.controller->requestAt(0.5, 0.5);
    run.frame(); // the click turns autofocus off and moves the lens; the throw comes after the move
    run.offerThrows = false;

    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_FALSE(run.last.controllable);
    EXPECT_EQ(run.last.position, std::nullopt);
    EXPECT_TRUE(run.log->autoFocus);
    EXPECT_EQ(run.log->count("auto on"), 1);
    const std::size_t calls = run.log->calls.size();
    run.framesFor(5s);
    EXPECT_EQ(run.log->calls.size(), calls);
}

TEST(FocusController, EndPutsBackWhatWasFound) {
    { // (a) autofocus on at the start
        CameraRun run;
        ASSERT_TRUE(run.until(State::Searching));
        EXPECT_TRUE(run.controller->end());
        EXPECT_TRUE(run.log->autoFocus);
        const std::size_t calls = run.log->calls.size();
        EXPECT_TRUE(run.controller->end()); // a second end makes no call
        EXPECT_EQ(run.log->calls.size(), calls);
    }
    { // (b) autofocus off at 80
        CameraRun run;
        run.log->autoFocus = false;
        run.log->position = 80;
        ASSERT_TRUE(run.until(State::Searching));
        run.framesFor(500ms);
        EXPECT_TRUE(run.controller->end());
        EXPECT_FALSE(run.log->autoFocus);
        EXPECT_EQ(run.log->position, 80);
    }
    { // (c) the restore fails
        CameraRun run;
        ASSERT_TRUE(run.until(State::Searching));
        run.log->failSetAuto = true;
        EXPECT_FALSE(run.controller->end());
    }
    { // (d) found in manual focus with the position unreadable: nothing to put back
        CameraRun run;
        run.log->autoFocus = false;
        run.log->positionReadable = false;
        ASSERT_TRUE(run.until(State::Searching));
        run.framesFor(500ms);
        const std::size_t calls = run.log->calls.size();
        EXPECT_TRUE(run.controller->end());
        EXPECT_EQ(run.log->calls.size(), calls);
    }
}

TEST(FocusController, CoolDownOfThreeSeconds) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.curve.at = 40; // blurred again at once
    run.framesFor(2900ms);
    EXPECT_EQ(run.searches, 1);
    run.framesFor(2s);
    EXPECT_EQ(run.searches, 2);
}

TEST(FocusController, FailedLetGoRestoreIsReported) {
    CameraRun run;
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.log->failSetAuto = true;
    run.page = false;
    run.framesFor(1100ms);
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_TRUE(run.last.restoreFailed);
}

// --- Task 5: click-to-focus ---

const cv::Size kClickFrame{640, 360};

// An 8-pixel checkerboard in `where` (flat grey elsewhere), blurred with σ = 0.2 × the lens's distance from
// `peak`; no blur at the peak.
cv::Mat checkerboard(int lens, double peak, cv::Rect where = cv::Rect(0, 0, 640, 360)) {
    // Drawn once for each region and blur: thousands of frames ask for a few dozen pictures.
    static std::map<std::tuple<int, int, int, int, int>, cv::Mat> drawn;
    const double sigma = 0.2 * std::abs(lens - peak);
    const auto key =
        std::tuple{where.x, where.y, where.width, where.height, static_cast<int>(std::lround(sigma * 10))};
    if (const auto found = drawn.find(key); found != drawn.end()) {
        return found->second;
    }
    cv::Mat image(kClickFrame, CV_8UC3, cv::Scalar::all(128));
    for (int y = where.y; y < where.y + where.height; ++y) {
        for (int x = where.x; x < where.x + where.width; ++x) {
            image.at<cv::Vec3b>(y, x) =
                (x / 8 + y / 8) % 2 == 0 ? cv::Vec3b(30, 30, 30) : cv::Vec3b(225, 225, 225);
        }
    }
    if (sigma > 0.0) {
        cv::GaussianBlur(image, image, cv::Size(0, 0), sigma);
    }
    drawn.emplace(key, image);
    return image;
}

TEST(FocusController, ClickFocusesTheRegion) {
    CameraRun run;
    run.page = false;
    run.picture = [&run](int lens) { return checkerboard(lens, run.curve.at); };
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
    EXPECT_GE(*run.last.position, 25);
    EXPECT_LE(*run.last.position, 35);
    // "Before" is the region at the lens's own position, 120, before any move.
    const double before = doc_vision::regionSharpness(checkerboard(120, 30), cv::Rect(284, 144, 72, 72));
    EXPECT_EQ(run.last.note.rfind("Focused at ", 0), 0u) << run.last.note;
    EXPECT_NE(run.last.note.find(": " + std::to_string(std::lround(before)) + " → "), std::string::npos)
        << run.last.note;
}

TEST(FocusController, ClickSaysFocusing) {
    CameraRun run;
    run.page = false;
    run.picture = [&run](int lens) { return checkerboard(lens, run.curve.at); };
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    run.frame();
    EXPECT_EQ(run.last.state, State::Searching);
    EXPECT_EQ(run.last.note, "Focusing…");
}

TEST(FocusController, ClickOnAFlatRegionFails) {
    CameraRun run;
    run.page = false;
    run.picture = [](int) { return cv::Mat(kClickFrame, CV_8UC3, cv::Scalar::all(128)); };
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    EXPECT_EQ(run.searchToTheEnd(), State::Camera);
    EXPECT_EQ(run.last.note, "Could not focus there");
    EXPECT_TRUE(run.log->autoFocus);
}

TEST(FocusController, ClickAbortsAPageSearch) {
    CameraRun run;
    run.picture = [&run](int lens) { return checkerboard(lens, run.curve.at); };
    ASSERT_TRUE(run.until(State::Searching));
    ASSERT_EQ(run.last.kind, FocusStatus::Kind::Page);
    run.controller->requestAt(0.5, 0.5);
    run.frame();
    EXPECT_EQ(run.last.state, State::Searching);
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
}

TEST(FocusController, ClickNoPageSearchMeanwhile) {
    CameraRun run;
    run.curve.at = 200; // the page stays blurred near 30, where the checkerboard is sharp
    run.picture = [](int lens) { return checkerboard(lens, 30); };
    run.framesFor(1s);
    run.controller->requestAt(0.5, 0.5);
    run.frame();
    while (run.last.state == State::Searching && run.at < 20s) {
        EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
        run.frame();
    }
    EXPECT_EQ(run.last.state, State::Holding);
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
}

TEST(FocusController, ClickWithoutAControl) {
    CameraRun run(false);
    run.page = false;
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    run.frame();
    EXPECT_EQ(run.last.note, "This camera's focus can't be set from the lab");
    EXPECT_EQ(run.last.state, State::Camera);
}

TEST(FocusController, ClickIgnoresNaN) {
    CameraRun run;
    run.page = false;
    run.picture = [](int lens) { return checkerboard(lens, 30, cv::Rect(560, 0, 80, 80)); };
    run.frame();
    const std::size_t calls = run.log->calls.size();
    run.controller->requestAt(std::nan(""), 0.5);
    run.controller->requestAt(0.5, INFINITY);
    run.framesFor(1s);
    EXPECT_EQ(run.log->calls.size(), calls);
    EXPECT_EQ(run.last.state, State::Camera);
    run.controller->requestAt(2.0, -1.0); // clamped: the top-right corner, where the checkerboard is
    EXPECT_EQ(run.searchToTheEnd(), State::Holding);
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
}

TEST(FocusController, ClickLastRequestWins) {
    CameraRun run;
    run.page = false;
    run.picture = [](int lens) { return checkerboard(lens, 30, cv::Rect(320, 0, 320, 360)); };
    run.frame();
    run.controller->requestAt(0.1, 0.5); // flat grey
    run.controller->requestAt(0.9, 0.5); // the checkerboard
    EXPECT_EQ(run.searchToTheEnd(), State::Holding);
}

TEST(FocusController, ClickOnASixteenBitFrame) {
    CameraRun run;
    run.page = false;
    run.tags.valueRange = {.min = 0.0, .max = 4095.0};
    run.picture = [](int lens) {
        cv::Mat grey;
        cv::cvtColor(checkerboard(lens, 30), grey, cv::COLOR_BGR2GRAY);
        cv::Mat twelveBits;
        grey.convertTo(twelveBits, CV_16UC1, 4095.0 / 255.0);
        return twelveBits;
    };
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    EXPECT_EQ(run.searchToTheEnd(), State::Holding);
}

TEST(FocusController, ClickHoldOutlastsAPageLoss) {
    CameraRun run;
    run.curve.at = 200;
    run.picture = [](int lens) { return checkerboard(lens, 30); };
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.page = false;
    run.framesFor(2s);
    EXPECT_EQ(run.last.state, State::Holding);
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
}

TEST(FocusController, ClickSearchCoolsDownPageSearches) {
    CameraRun run;
    run.curve.at = 200; // the page blurred where the click focuses
    run.picture = [](int lens) { return checkerboard(lens, 30); };
    run.frame();
    run.controller->requestAt(0.5, 0.5);
    ASSERT_EQ(run.searchToTheEnd(), State::Holding);
    run.framesFor(2900ms);
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Click);
    EXPECT_TRUE(run.until(State::Searching, 3s));
    EXPECT_EQ(run.last.kind, FocusStatus::Kind::Page);
}

// Final review F2: a search aborted because the page changed size holds its passing best, then lets go at
// once: the page is no longer the size the search was for.
TEST(FocusController, AbortForASizeChangeLetsGo) {
    CameraRun run;
    ASSERT_TRUE(run.until(State::Searching));
    const auto measured30 = [&] {
        for (const lab_core::FocusStep& step : run.controller->curve()) {
            if (step.position == 30) {
                return true;
            }
        }
        return false;
    };
    while (!measured30() && run.at < 20s) {
        run.frame();
    }
    run.side = 0.48 * 1.25;
    run.frame(); // the abort: 30 passed, so it is held
    run.frame();
    EXPECT_EQ(run.last.state, State::Camera);
    EXPECT_TRUE(run.log->autoFocus);
}

// Final review F3: one noisy sharp frame after a failed search does not lose "can't focus" for good.
TEST(FocusController, CannotFocusComesBackAfterANoisyFrame) {
    CameraRun run;
    run.curve.peak = 36;
    ASSERT_EQ(run.searchToTheEnd(), State::CannotFocus);
    run.sharpFlicker = true;
    run.frame();
    ASSERT_EQ(run.last.state, State::Camera);
    run.framesFor(3100ms);
    EXPECT_EQ(run.last.state, State::CannotFocus);
    EXPECT_TRUE(run.last.afterSearch);
    EXPECT_EQ(run.searches, 1);
}

} // namespace
