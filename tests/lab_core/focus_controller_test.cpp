#include "lab_core/focus_controller.h"

#include "fake_focus_control.h"

#include <doc_vision/guidance.h>
#include <doc_vision/quality_gates.h>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    double operator()(int position) const {
        const double d = (position - at) / width;
        return floor + peak * std::exp(-d * d);
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
    bool numbered = false;              // each frame's sharpness 1000 + its number
    std::optional<std::uint64_t> offer; // what nextOffer answers instead of the next frame's number
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
        const double s = sharpness(at);
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
        return facts;
    }

    FocusStatus frame() {
        if (!m_startPosition) {
            m_startPosition = log->position;
        }
        log->now = at;
        frame_io::Frame frame{.image = image, .number = number, .at = at, .tags = {}};
        const State before = last.state;
        last = controller->next(frame, facts());
        if (last.state == State::Searching && before != State::Searching) {
            ++searches;
        }
        ++number;
        at += kFrameTime;
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

    cv::Mat image; // the frames' picture: empty unless a click test draws one

private:
    std::uint64_t nextOffer() const { return offer ? *offer : number + 1; }
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

} // namespace
