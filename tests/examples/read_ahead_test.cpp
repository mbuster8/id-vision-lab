#include "live_sender.h"
#include "read_ahead.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using live_sender::Playback;
using live_sender::ReadAhead;

TEST(ReadAhead, KeepsTheOrderAndEnds) {
    int next = 0; // only the reading thread touches it
    ReadAhead<int> ahead(
        [&next]() -> std::optional<int> { return next < 10 ? std::optional(next++) : std::nullopt; }, 3);

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(ahead.take(), std::optional(i));
    }
    EXPECT_EQ(ahead.take(), std::nullopt);
    EXPECT_EQ(ahead.take(), std::nullopt); // and stays ended
}

// It waits for room before it reads, so it never holds more than `depth` read and not taken.
TEST(ReadAhead, ReadsAtMostDepthAhead) {
    std::atomic<int> reads{0};
    ReadAhead<int> ahead([&reads]() -> std::optional<int> { return reads++; }, 2);

    EXPECT_EQ(ahead.take(), std::optional(0));
    std::this_thread::sleep_for(50ms); // time for the reader to fill up again

    EXPECT_LE(reads.load(), 3); // the one taken and two ahead
    EXPECT_GE(reads.load(), 1);
}

TEST(ReadAhead, DestructionDoesNotWaitForTheEnd) {
    std::atomic<int> reads{0};
    {
        ReadAhead<int> ahead([&reads]() -> std::optional<int> { return reads++; }, 3); // never ends
        ASSERT_TRUE(ahead.take().has_value());
    } // returns: the test would hang otherwise
    const int afterwards = reads.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(reads.load(), afterwards); // the thread has stopped
}

// A reader that throws ends the stream instead of the program.
TEST(ReadAhead, AThrowingReadEndsTheStream) {
    ReadAhead<int> ahead([]() -> std::optional<int> { throw std::runtime_error("decoder failed"); }, 2);

    EXPECT_EQ(ahead.take(), std::nullopt);
}

const Playback::Clock::time_point kStart = Playback::Clock::time_point{} + 1000s;

TEST(Playback, OnTimeIsNotLate) {
    const Playback playback(kStart, 40ms);

    EXPECT_EQ(playback.due(1s), kStart + 1s);
    EXPECT_FALSE(playback.late(1s, kStart + 1s + 20ms));
    EXPECT_FALSE(playback.late(1s, kStart + 1s + 40ms)); // a whole interval late is still shown
}

TEST(Playback, SkipsAFrameMoreThanAnIntervalLate) {
    const Playback playback(kStart, 40ms);

    EXPECT_TRUE(playback.late(1s, kStart + 1s + 41ms));
}

} // namespace
