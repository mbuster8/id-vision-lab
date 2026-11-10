#include "frame_io/read_ahead.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using frame_io::ReadAhead;

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

// Polls `ready` for up to a second; the reader runs on its own thread.
bool becomesReady(const ReadAhead<int>& ahead) {
    for (int i = 0; i < 1000 && !ahead.ready(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    return ahead.ready();
}

// ready() says whether take() would return at once: an item waits, or the stream has ended.
TEST(ReadAhead, ReadyWhenAnItemWaitsOrTheStreamEnded) {
    std::promise<void> go;
    const std::shared_future<void> released = go.get_future().share();
    int next = 0; // only the reading thread touches it
    ReadAhead<int> ahead(
        [&released, &next]() -> std::optional<int> {
            released.wait();
            return next < 1 ? std::optional(next++) : std::nullopt;
        },
        2);

    const bool readyBeforeTheRead = ahead.ready();
    go.set_value(); // before any assertion, so the reader never waits for ever

    EXPECT_FALSE(readyBeforeTheRead);
    EXPECT_TRUE(becomesReady(ahead));
    EXPECT_EQ(ahead.take(), std::optional(0));
    EXPECT_TRUE(becomesReady(ahead));
    EXPECT_EQ(ahead.take(), std::nullopt);
}

// The wake is called on the reading thread after each item and at the end, so a caller can wait for a frame
// and for anything else on one condition of its own.
TEST(ReadAhead, WakesWhenAnItemIsReadyAndAtTheEnd) {
    std::promise<void> go;
    const std::shared_future<void> released = go.get_future().share();
    int next = 0; // only the reading thread touches it
    ReadAhead<int> ahead(
        [&released, &next]() -> std::optional<int> {
            released.wait();
            return next < 2 ? std::optional(next++) : std::nullopt;
        },
        4);
    std::atomic<int> wakes{0};
    ahead.setWake([&wakes] { ++wakes; });

    go.set_value();
    EXPECT_EQ(ahead.take(), std::optional(0));
    EXPECT_EQ(ahead.take(), std::optional(1));
    EXPECT_EQ(ahead.take(), std::nullopt);

    EXPECT_EQ(wakes.load(), 3); // two items, then the end
}

// A reader that throws ends the stream instead of the program.
TEST(ReadAhead, AThrowingReadEndsTheStream) {
    ReadAhead<int> ahead([]() -> std::optional<int> { throw std::runtime_error("decoder failed"); }, 2);

    EXPECT_EQ(ahead.take(), std::nullopt);
}

// With a handler, the exception becomes the stream's last item, so a replay can stop with an error rather
// than look as if it had ended.
TEST(ReadAhead, AThrowingReadGivesTheHandlersItem) {
    int calls = 0;    // only the reading thread touches it
    std::string what; // set by the handler before its item is taken
    ReadAhead<int> ahead(
        [&calls]() -> std::optional<int> {
            if (++calls == 3) {
                throw std::runtime_error("boom");
            }
            return calls;
        },
        2,
        [&what](std::string thrown) -> std::optional<int> {
            what = std::move(thrown);
            return -1;
        });

    EXPECT_EQ(ahead.take(), std::optional(1));
    EXPECT_EQ(ahead.take(), std::optional(2));
    EXPECT_EQ(ahead.take(), std::optional(-1));
    EXPECT_EQ(what, "boom");
    EXPECT_EQ(ahead.take(), std::nullopt);
}

// Anything thrown, not only a standard exception.
TEST(ReadAhead, ANonStandardThrowEndsTheStream) {
    int calls = 0;
    ReadAhead<int> ahead(
        [&calls]() -> std::optional<int> {
            if (++calls == 3) {
                throw 42;
            }
            return calls;
        },
        2);

    EXPECT_EQ(ahead.take(), std::optional(1));
    EXPECT_EQ(ahead.take(), std::optional(2));
    EXPECT_EQ(ahead.take(), std::nullopt);
}

} // namespace
