#include "live_sender.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>

namespace {

using namespace std::chrono_literals;
using live_sender::Playback;
using live_sender::RealTimeFrames;
using live_sender::TimedFrame;

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

// A video of `count` frames at 25 fps, each holding its number, read on a clock the test moves.
struct FakeVideo {
    Playback::Clock::time_point now = kStart;
    int count = 0;
    int index = 0;
    Playback::Clock::duration readTime{}; // how long reading each frame takes
    bool ready = true;                    // whether a frame waits when asked for

    RealTimeFrames frames() {
        return RealTimeFrames([this] { return ready; },
                              [this]() -> std::optional<TimedFrame> {
                                  now += readTime;
                                  if (index == count) {
                                      return std::nullopt;
                                  }
                                  const int number = index++;
                                  return TimedFrame{cv::Mat(1, 1, CV_8UC1, cv::Scalar(number)),
                                                    40ms * number};
                              },
                              40ms, [this] { return now; },
                              [this](Playback::Clock::time_point due) {
                                  now = std::max(now, due);
                                  return true;
                              });
    }
};

// The frames shown, by number; `sendTime` is how long sending each one takes.
std::vector<int> shown(RealTimeFrames& frames, FakeVideo& video, Playback::Clock::duration sendTime = {}) {
    std::vector<int> numbers;
    while (const std::optional<cv::Mat> frame = frames.next()) {
        numbers.push_back(frame->at<uchar>(0, 0));
        video.now += sendTime;
    }
    return numbers;
}

// (F3) When the reading is what falls behind, skipping cannot catch up: every frame after the first few would
// be skipped and the picture would freeze. The video plays slower instead.
TEST(RealTimeFrames, ASlowReaderSlowsTheVideoInsteadOfSkipping) {
    FakeVideo video{.count = 5, .readTime = 100ms, .ready = false};
    RealTimeFrames frames = video.frames();

    EXPECT_EQ(shown(frames, video), (std::vector<int>{0, 1, 2, 3, 4}));
    EXPECT_EQ(frames.skipped(), 0);
}

// When the sending falls behind, a frame that was ready but is more than a frame late is skipped, so the
// video keeps its time: each send takes 100 ms, two and a half frames.
TEST(RealTimeFrames, SkipsAReadyFrameMoreThanAFrameLate) {
    FakeVideo video{.count = 10};
    RealTimeFrames frames = video.frames();

    EXPECT_EQ(shown(frames, video, 100ms), (std::vector<int>{0, 2, 4, 7, 9}));
    EXPECT_EQ(frames.skipped(), 5);
}

TEST(RealTimeFrames, WithoutAnIntervalComesAsRead) {
    FakeVideo video{.count = 3};
    int waits = 0;
    RealTimeFrames frames([] { return true; },
                          [&video]() -> std::optional<TimedFrame> {
                              if (video.index == video.count) {
                                  return std::nullopt;
                              }
                              const int number = video.index++;
                              return TimedFrame{cv::Mat(1, 1, CV_8UC1, cv::Scalar(number)), {}};
                          },
                          std::nullopt, [&video] { return video.now; },
                          [&waits](Playback::Clock::time_point) {
                              ++waits;
                              return true;
                          });

    EXPECT_EQ(shown(frames, video, 1s), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(waits, 0);
}

} // namespace
