#include "lab_core/viewer_link.h"

#include "fake_sender.h"
#include "latch.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lab_core::ViewerLink;
using test_support::RecordingSender;
using visual_debug::SendResult;
using visual_debug::ViewerState;

// Fakes only: no request leaves the process.
constexpr char kFirst[] = "tcp://127.0.0.1:7201";
constexpr char kSecond[] = "tcp://127.0.0.1:7202";
constexpr char kThird[] = "tcp://127.0.0.1:7203";

// Makes fake senders, noting each endpoint; every call goes into one log.
struct Factory {
    std::vector<std::string> endpoints;
    std::function<ViewerState()> pingAnswer = [] { return ViewerState::Listening; };
    std::function<SendResult(std::string_view)> sendAnswer = [](std::string_view) {
        return SendResult::Delivered;
    };
    test_support::SendLog log;

    ViewerLink::MakeSender make() {
        return [this](const std::string& endpoint) -> std::unique_ptr<visual_debug::ImageSender> {
            endpoints.push_back(endpoint);
            auto sender =
                std::make_unique<RecordingSender>([this](std::string_view name) { return sendAnswer(name); });
            sender->pingAnswer = [this] { return pingAnswer(); };
            sender->log = &log;
            return sender;
        };
    }
};

// A link pumped by the test, with a clock the test moves.
struct Harness {
    ViewerLink::Clock::time_point now = ViewerLink::Clock::time_point{} + 1000s;
    int changes = 0;
    std::function<void()> onChanged;
    Factory factory;
    ViewerLink link{ViewerLink::Mode::Immediate,
                    factory.make(),
                    {.changed =
                         [this] {
                             ++changes;
                             if (onChanged) {
                                 onChanged();
                             }
                         },
                     .now = [this] { return now; }}};
};

cv::Mat still() { return cv::Mat(3, 4, CV_8UC3, cv::Scalar::all(9)); }

std::vector<std::string> linesOfKind(const test_support::SendLog& log, const std::string& kind) {
    std::vector<std::string> lines;
    for (const std::string& line : log.lines()) {
        if (line.rfind(kind + " ", 0) == 0) {
            lines.push_back(line);
        }
    }
    return lines;
}

TEST(ViewerLink, PingsAtOnceThenEveryTwoSeconds) {
    Harness h;
    h.link.setEndpoint(kFirst);

    EXPECT_TRUE(h.link.pump());
    EXPECT_EQ(h.link.state().viewer, ViewerState::Listening);
    EXPECT_EQ(h.link.state().endpoint, kFirst);
    EXPECT_FALSE(h.link.pump());
    h.now += 1999ms;
    EXPECT_FALSE(h.link.pump());
    h.now += 1ms;
    EXPECT_TRUE(h.link.pump());

    EXPECT_EQ(linesOfKind(h.factory.log, "ping").size(), 2U);
    EXPECT_EQ(h.factory.endpoints, std::vector<std::string>{kFirst}); // one connection, kept
}

// An older viewer logs each ping as a rejected request: once a minute is enough to notice it was replaced.
TEST(ViewerLink, OlderViewerIsAskedEveryMinute) {
    Harness h;
    h.factory.pingAnswer = [] { return ViewerState::PredatesPing; };
    h.link.setEndpoint(kFirst);
    EXPECT_TRUE(h.link.pump());
    EXPECT_EQ(h.link.state().viewer, ViewerState::PredatesPing);

    h.now += 2s;
    EXPECT_FALSE(h.link.pump());
    h.now += 58s;
    EXPECT_TRUE(h.link.pump());
}

// Stills go before pings, in order, as ordinary images; a fifth while four wait is refused and counted.
TEST(ViewerLink, StillsGoInOrderAndFourWait) {
    Harness h;
    h.link.setEndpoint(kFirst);
    for (int i = 1; i <= 5; ++i) {
        h.link.sendStill(still(), "still " + std::to_string(i), {});
    }
    EXPECT_EQ(h.link.state().stillsRefused, 1U);

    while (h.link.pump()) {
    }

    EXPECT_EQ(h.factory.log.lines(), (std::vector<std::string>{"image still 1", "image still 2",
                                                               "image still 3", "image still 4", "ping "}));
    EXPECT_EQ(h.link.state().lastStill, SendResult::Delivered);
    EXPECT_EQ(h.link.state().lastStillName, "still 4");
}

// The stills waiting were for the old viewer, and so is an answer that comes after the change.
TEST(ViewerLink, NewEndpointDropsStillsAndOldAnswers) {
    Harness h;
    h.link.setEndpoint(kFirst);
    h.link.sendStill(still(), "a", {});
    h.link.sendStill(still(), "b", {});

    h.link.setEndpoint(kSecond);
    EXPECT_EQ(h.link.state().stillsRefused, 2U);
    EXPECT_EQ(h.link.state().endpoint, kSecond);

    bool switched = false;
    h.factory.pingAnswer = [&] {
        if (!switched) {
            switched = true;
            h.link.setEndpoint(kThird); // while the ping to the second is in progress
        }
        return ViewerState::Listening;
    };
    EXPECT_TRUE(h.link.pump());
    EXPECT_EQ(h.link.state().endpoint, kThird);
    EXPECT_EQ(h.link.state().viewer, std::nullopt);

    EXPECT_TRUE(h.link.pump());
    EXPECT_EQ(h.link.state().viewer, ViewerState::Listening);
    EXPECT_EQ(h.factory.endpoints, (std::vector<std::string>{kSecond, kThird}));
    EXPECT_TRUE(linesOfKind(h.factory.log, "image").empty());
}

// Closing the window stops the link without waiting for a slow viewer; the window then waits for idle().
TEST(ViewerLink, StopDropsAndIsIdle) {
    test_support::Latch latch;
    Factory factory;
    factory.pingAnswer = [&latch] {
        latch.hold();
        return ViewerState::Listening;
    };
    ViewerLink link(ViewerLink::Mode::Threaded, factory.make(), {.changed = nullptr, .now = nullptr});
    link.setEndpoint(kFirst);
    ASSERT_TRUE(latch.waitUntilEntered()); // the first ping waits for its answer
    link.sendStill(still(), "a", {});
    link.sendStill(still(), "b", {});

    const auto started = std::chrono::steady_clock::now();
    link.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_FALSE(link.idle());
    latch.release();

    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!link.idle() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(link.idle());
    EXPECT_TRUE(linesOfKind(factory.log, "image").empty());
}

TEST(ViewerLink, EmptyEndpointNeverPings) {
    Harness h;
    EXPECT_FALSE(h.link.pump());
    h.link.setEndpoint("");
    h.now += 10s;
    EXPECT_FALSE(h.link.pump());

    h.link.sendStill(still(), "a", {});
    EXPECT_FALSE(h.link.pump());
    EXPECT_EQ(h.link.state().stillsRefused, 1U);
    EXPECT_TRUE(h.factory.endpoints.empty());
    EXPECT_TRUE(h.link.idle());
}

// The window reads the state from its hook: it is there already, and no lock is held.
TEST(ViewerLink, ChangedHookWithNoLockHeld) {
    Harness h;
    std::optional<ViewerState> seen;
    h.onChanged = [&] { seen = h.link.state().viewer; };
    h.link.setEndpoint(kFirst);

    h.link.pump();

    EXPECT_EQ(h.changes, 1);
    EXPECT_EQ(seen, ViewerState::Listening);
}

// A4: a still's number tells its answer from another's, so a window waiting for one never takes an earlier
// one's answer for it.
TEST(ViewerLink, StillNumbersCountUp) {
    Harness h;
    h.link.setEndpoint(kFirst);
    h.factory.sendAnswer = [](std::string_view name) {
        return name == "a" ? SendResult::ViewerUnavailable : SendResult::Delivered;
    };

    EXPECT_EQ(h.link.sendStill(still(), "a", {}), 1U);
    EXPECT_EQ(h.link.sendStill(still(), "b", {}), 2U);
    EXPECT_TRUE(h.link.pump());
    EXPECT_EQ(h.link.state().lastStillNumber, 1U);
    EXPECT_EQ(h.link.state().lastStill, SendResult::ViewerUnavailable);

    EXPECT_TRUE(h.link.pump());

    EXPECT_EQ(h.link.state().lastStillNumber, 2U);
    EXPECT_EQ(h.link.state().lastStill, SendResult::Delivered);
    EXPECT_EQ(h.link.state().lastStillReason, "");
}

// A still waiting, or in progress, when the viewer changes is reported, not left waiting for ever.
TEST(ViewerLink, DroppedStillIsReported) {
    Harness h;
    h.link.setEndpoint(kFirst);
    const std::uint64_t waiting = h.link.sendStill(still(), "a", {});

    h.link.setEndpoint(kSecond);

    EXPECT_EQ(h.link.state().lastStillNumber, waiting);
    EXPECT_EQ(h.link.state().lastStill, std::nullopt);
    EXPECT_EQ(h.link.state().lastStillReason, "the viewer changed");

    std::uint64_t inProgress = 0;
    h.factory.sendAnswer = [&](std::string_view) {
        h.link.setEndpoint(kThird); // while the still to the second is in progress
        return SendResult::Delivered;
    };
    inProgress = h.link.sendStill(still(), "b", {});
    EXPECT_TRUE(h.link.pump());

    EXPECT_EQ(h.link.state().lastStillNumber, inProgress);
    EXPECT_EQ(h.link.state().lastStill, std::nullopt);
    EXPECT_EQ(h.link.state().lastStillReason, "the viewer changed");
}

TEST(ViewerLink, StopReportsTheStillsItDrops) {
    Harness h;
    h.link.setEndpoint(kFirst);
    const std::uint64_t waiting = h.link.sendStill(still(), "a", {});

    h.link.stop();

    EXPECT_EQ(h.link.state().lastStillNumber, waiting);
    EXPECT_EQ(h.link.state().lastStillReason, "the viewer changed");
}

TEST(ViewerLink, RefusedStillIsReported) {
    Harness h;
    h.link.setEndpoint(kFirst);
    for (int i = 0; i < 4; ++i) {
        h.link.sendStill(still(), "waits", {});
    }

    const std::uint64_t refused = h.link.sendStill(still(), "fifth", {});

    EXPECT_EQ(refused, 5U);
    EXPECT_EQ(h.link.state().lastStillNumber, refused);
    EXPECT_EQ(h.link.state().lastStill, std::nullopt);
    EXPECT_EQ(h.link.state().lastStillReason, "refused: too many stills waiting");

    Harness none;
    const std::uint64_t noViewer = none.link.sendStill(still(), "a", {});
    EXPECT_EQ(none.link.state().lastStillNumber, noViewer);
    EXPECT_EQ(none.link.state().lastStillReason, "refused: no viewer address");
}

} // namespace
