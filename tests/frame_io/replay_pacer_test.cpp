#include "frame_io/replay_pacer.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;
using Clock = frame_io::ReplayPacer::Clock;

const Clock::time_point t0 = Clock::time_point{} + 1000s;

// Each frame is due its gap after the one before was due, so frames that are ready early wait their turn.
TEST(ReplayPacer, KeepsTheFramesSpacing) {
    frame_io::ReplayPacer pacer;

    EXPECT_EQ(pacer.due(0ms, t0), t0);
    EXPECT_EQ(pacer.due(33ms, t0 + 1ms), t0 + 33ms);
    EXPECT_EQ(pacer.due(66ms, t0 + 40ms), t0 + 66ms);
}

// A frame ready after it was due (processing was slow) is due when ready, and the schedule goes on from it:
// the replay plays slower, never rushes to catch up, and never skips.
TEST(ReplayPacer, StartsAgainAfterAStall) {
    frame_io::ReplayPacer pacer;
    pacer.due(0ms, t0);
    pacer.due(33ms, t0 + 1ms);
    pacer.due(66ms, t0 + 40ms);

    EXPECT_EQ(pacer.due(99ms, t0 + 200ms), t0 + 200ms);
    EXPECT_EQ(pacer.due(132ms, t0 + 201ms), t0 + 233ms);
}

TEST(ReplayPacer, RestartForgets) {
    frame_io::ReplayPacer pacer;
    pacer.due(0ms, t0);
    const Clock::time_point t1 = t0 + 10s;

    pacer.restart();

    EXPECT_EQ(pacer.due(500ms, t1), t1);
    EXPECT_EQ(pacer.due(533ms, t1), t1 + 33ms);
}

} // namespace
