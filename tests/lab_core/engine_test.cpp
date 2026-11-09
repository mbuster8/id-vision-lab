#include "lab_core/engine.h"

#include "fake_sender.h"
#include "latch.h"
#include "list_source.h"
#include "scratch_folder.h"

#include <frame_io/recording.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frame_io::ReadResult;
using frame_io::ReadStatus;
using frame_io::RecordingWriter;
using lab_core::Clock;
using lab_core::Engine;
using lab_core::EngineState;

using test_support::framesApart;
using test_support::ListSource;
using test_support::openerOf;

// A 4×3 picture of the frame's number.
frame_io::Frame frameOf(std::uint64_t number, std::chrono::nanoseconds at) {
    return test_support::listFrame(number, at);
}

constexpr std::size_t kFrameBytes = 4 * 3 * 3;

// Counts its calls; may throw at a frame or write into its input. Each result's finding is its name.
class CountingProcessor : public lab_core::FrameProcessor {
public:
    struct Calls {
        std::vector<std::uint64_t> processed;
        int resets = 0;
    };

    explicit CountingProcessor(Calls& calls, std::string name = "counting")
        : m_calls(calls), m_name(std::move(name)) {}

    lab_core::ProcessorResult process(const frame_io::Frame& frame) override {
        m_calls.processed.push_back(frame.number);
        if (throwAt == frame.number) {
            throw std::runtime_error("processor failed");
        }
        if (writeInput) {
            cv::Mat alias = frame.image; // a copy of the header shares the pixels
            alias.setTo(cv::Scalar::all(1));
        }
        return {.overlays = {}, .images = {}, .findings = {m_name}};
    }
    void reset() override { ++m_calls.resets; }

    std::optional<std::uint64_t> throwAt;
    bool writeInput = false;

private:
    Calls& m_calls;
    std::string m_name;
};

// An engine pumped by the test, with a clock the test moves, counting its hooks.
struct Harness {
    Clock::time_point now = Clock::time_point{} + 1000s;
    int resultsReady = 0;
    int statusChanges = 0;
    Engine engine{Engine::Mode::Immediate,
                  {.resultReady = [this] { ++resultsReady; },
                   .statusChanged = [this] { ++statusChanges; },
                   .now = [this] { return now; }}};

    // Pumps once and takes the result, if any.
    std::optional<lab_core::LabResult> next() {
        engine.pump();
        return engine.takeResult();
    }
    std::optional<std::uint64_t> nextNumber() {
        const auto result = next();
        return result ? std::optional(result->frame.number) : std::nullopt;
    }
};

lab_core::PendingFrame pending(std::uint64_t closureNumber, std::chrono::nanoseconds at) {
    return {[closureNumber, at](std::string&) -> std::optional<frame_io::Frame> {
        return frameOf(closureNumber, at);
    }};
}

RecordingWriter::Options immediateRecorder() {
    RecordingWriter::Options options;
    options.mode = RecordingWriter::Mode::Immediate;
    options.freeSpace = [](const std::filesystem::path&) { return std::uintmax_t{100'000'000'000}; };
    return options;
}

std::unique_ptr<RecordingWriter> recorderIn(const std::filesystem::path& folder,
                                            RecordingWriter::Options options = immediateRecorder()) {
    std::string error;
    auto writer = RecordingWriter::start(
        folder, {.kind = "video", .name = "list", .format = {}, .startedAt = {}}, std::move(options), error);
    EXPECT_TRUE(writer) << error;
    return writer;
}

std::uint64_t framesRecordedIn(const std::filesystem::path& folder) {
    std::string error;
    const auto recording = frame_io::openRecording(folder, error);
    EXPECT_TRUE(recording) << error;
    return recording ? recording->frameCount().value_or(0) : 0;
}

// --- Live
// ---------------------------------------------------------------------------------------------------

// The newest frame is processed; the ones it replaced are counted, and leave gaps in the numbers, which count
// the frames offered whatever number the conversion gives.
TEST(Engine, LiveKeepsTheNewest) {
    Harness h;
    h.engine.startLive("Test camera");
    h.engine.offer(pending(100, 0ms));
    h.engine.offer(pending(101, 33ms));
    h.engine.offer(pending(102, 66ms));

    EXPECT_TRUE(h.engine.pump());

    auto result = h.engine.takeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->frame.number, 2U);
    EXPECT_EQ(result->frame.at, 66ms);
    EXPECT_EQ(h.engine.status().state, EngineState::Live);
    EXPECT_EQ(h.engine.status().droppedBeforeProcessing, 2U);
    EXPECT_EQ(h.engine.status().source, "Test camera");
    h.engine.offer(pending(103, 99ms));
    EXPECT_EQ(h.nextNumber(), 3U);
}

// --- Replay
// -------------------------------------------------------------------------------------------------

TEST(Engine, ReplayKeepsEveryFrameInOrder) {
    Harness h;
    h.engine.startReplay(openerOf(framesApart(5, 40ms)));

    std::vector<std::uint64_t> shown;
    for (int i = 0; i < 5; ++i) {
        const auto number = h.nextNumber();
        ASSERT_TRUE(number) << "frame " << i;
        shown.push_back(*number);
        h.now += 40ms;
    }

    EXPECT_EQ(shown, (std::vector<std::uint64_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(h.engine.status().state, EngineState::Playing);
    EXPECT_TRUE(h.engine.pump()); // reads the end
    EXPECT_EQ(h.engine.status().state, EngineState::Ended);
    EXPECT_EQ(h.engine.status().frameNumber, 4U);
    EXPECT_EQ(h.engine.status().frameCount, 5U);
}

// A frame waits for its time; one ready late is shown at once, and the schedule goes on from it.
TEST(Engine, ReplayWaitsForItsTime) {
    Harness h;
    h.engine.startReplay(openerOf(framesApart(4, 40ms)));
    EXPECT_EQ(h.nextNumber(), 0U);

    h.now += 10ms;
    EXPECT_FALSE(h.engine.pump());
    EXPECT_FALSE(h.engine.takeResult());
    h.now += 30ms;
    EXPECT_EQ(h.nextNumber(), 1U);

    h.now += 140ms; // processing fell behind: frame 2 was due 100 ms ago
    EXPECT_EQ(h.nextNumber(), 2U);
    h.now += 39ms;
    EXPECT_EQ(h.nextNumber(), std::nullopt);
    h.now += 1ms;
    EXPECT_EQ(h.nextNumber(), 3U);
}

TEST(Engine, AsFastAsPossibleIgnoresTheClock) {
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(5, 1s)));

    for (std::uint64_t i = 0; i < 5; ++i) {
        EXPECT_EQ(h.nextNumber(), i);
    }
    EXPECT_TRUE(h.engine.status().asFastAsPossible);
}

// Paused, Step shows the next frame; Step back the one before, as a new run of the processor.
TEST(Engine, PauseStepAndStepBack) {
    Harness h;
    CountingProcessor::Calls calls;
    h.engine.setProcessor(std::make_unique<CountingProcessor>(calls));
    h.engine.startReplay(openerOf(framesApart(5, 40ms)));
    EXPECT_EQ(h.nextNumber(), 0U);

    h.engine.pause();
    h.now += 1s;
    EXPECT_EQ(h.nextNumber(), std::nullopt);
    EXPECT_EQ(h.engine.status().state, EngineState::Paused);
    EXPECT_FALSE(h.engine.status().canStepBack);

    h.engine.step();
    EXPECT_EQ(h.nextNumber(), 1U);
    EXPECT_EQ(h.engine.status().state, EngineState::Paused);
    EXPECT_TRUE(h.engine.status().canStepBack);

    const std::uint64_t generation = h.engine.status().generation;
    const int resets = calls.resets;
    h.engine.stepBack();
    const auto back = h.next();
    ASSERT_TRUE(back);
    EXPECT_EQ(back->frame.number, 0U);
    EXPECT_EQ(back->generation, generation + 1);
    EXPECT_EQ(calls.resets, resets + 1);
    EXPECT_EQ(h.engine.status().state, EngineState::Paused);

    h.engine.play();
    EXPECT_EQ(h.nextNumber(), 1U);
    EXPECT_EQ(h.engine.status().state, EngineState::Playing);
}

TEST(Engine, RestartAndLoop) {
    Harness h;
    h.engine.setLoop(true);
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    for (std::uint64_t i = 0; i < 3; ++i) {
        EXPECT_EQ(h.nextNumber(), i);
    }
    const std::uint64_t generation = h.engine.status().generation;

    EXPECT_TRUE(h.engine.pump()); // the end: round again
    const auto looped = h.next();
    ASSERT_TRUE(looped);
    EXPECT_EQ(looped->frame.number, 0U);
    EXPECT_EQ(looped->generation, generation + 1);
    EXPECT_EQ(h.engine.status().state, EngineState::Playing);

    EXPECT_EQ(h.nextNumber(), 1U);
    h.engine.restart();
    const auto restarted = h.next();
    ASSERT_TRUE(restarted);
    EXPECT_EQ(restarted->frame.number, 0U);
    EXPECT_EQ(restarted->generation, generation + 2);
}

// At the end of a replay, Play (and Space) plays it again from its first frame, as players do.
TEST(Engine, PlayAtTheEndStartsAgain) {
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    for (std::uint64_t i = 0; i < 3; ++i) {
        EXPECT_EQ(h.nextNumber(), i);
    }
    h.engine.pump();
    ASSERT_EQ(h.engine.status().state, EngineState::Ended);

    h.engine.play();

    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(h.engine.status().state, EngineState::Playing);
}

// A video file cannot seek: Restart opens it again, and Step back is not offered.
TEST(Engine, RestartReopensAVideo) {
    Harness h;
    ListSource::Counters counters;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms), false, &counters));
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(h.nextNumber(), 1U);
    EXPECT_FALSE(h.engine.status().canStepBack);

    h.engine.restart();

    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(counters.restarts, 1);
}

// --- Seeking
// ------------------------------------------------------------------------------------------------

// A replay as fast as possible, paused with frame `shown` on screen.
void pausedAt(Harness& h, std::uint64_t shown) {
    h.engine.setAsFastAsPossible(true);
    for (std::uint64_t number = 0; number <= shown; ++number) {
        ASSERT_EQ(h.nextNumber(), number);
    }
    h.engine.pause();
    h.engine.pump();
    ASSERT_EQ(h.engine.status().state, EngineState::Paused);
}

// Pumps until a result comes, or twenty pumps.
std::optional<std::uint64_t> pumpUntilAResult(Harness& h) {
    for (int pumps = 0; pumps < 20; ++pumps) {
        if (const auto number = h.nextNumber()) {
            return number;
        }
    }
    return std::nullopt;
}

TEST(Engine, SeekWhilePausedShowsThatFrame) {
    Harness h;
    ListSource::Counters counters;
    h.engine.startReplay(openerOf(framesApart(10, 40ms), true, &counters));
    pausedAt(h, 2);
    const std::uint64_t generation = h.engine.status().generation;

    h.engine.seek(7);
    const auto result = h.next();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->frame.number, 7U);
    EXPECT_EQ(result->generation, generation + 1);
    EXPECT_EQ(h.engine.status().state, EngineState::Paused);
    EXPECT_EQ(counters.lastSeek, std::optional<std::uint64_t>(7));
}

TEST(Engine, SeekWhilePlayingPlaysOn) {
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(10, 40ms)));
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(h.nextNumber(), 1U);

    h.engine.seek(7);

    EXPECT_EQ(h.nextNumber(), 7U);
    EXPECT_EQ(h.nextNumber(), 8U);
    EXPECT_EQ(h.engine.status().state, EngineState::Playing);
}

TEST(Engine, SeekFromTheEndPauses) {
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    for (std::uint64_t number = 0; number < 3; ++number) {
        EXPECT_EQ(h.nextNumber(), number);
    }
    h.engine.pump();
    ASSERT_EQ(h.engine.status().state, EngineState::Ended);

    h.engine.seek(1);

    EXPECT_EQ(h.nextNumber(), 1U);
    EXPECT_EQ(h.engine.status().state, EngineState::Paused);
}

// Holding ← makes Step Backs faster than a video can seek: those waiting add up to one seek.
TEST(Engine, TenStepBacksMakeOneSeek) {
    Harness h;
    ListSource::Counters counters;
    h.engine.startReplay(openerOf(framesApart(15, 40ms), true, &counters));
    pausedAt(h, 12);

    for (int i = 0; i < 10; ++i) {
        h.engine.seekBy(-1);
    }

    EXPECT_EQ(pumpUntilAResult(h), 2U);
    EXPECT_EQ(counters.seeks, 1);
}

// A seek replaces the steps waiting before it; steps after it count from its frame. Before the first frame or
// after the last, a seek stops there.
TEST(Engine, AbsoluteSeekThenSteps) {
    Harness h;
    h.engine.startReplay(openerOf(framesApart(15, 40ms)));
    pausedAt(h, 12);

    h.engine.seekBy(-3);
    h.engine.seek(5);
    h.engine.seekBy(+2);
    EXPECT_EQ(pumpUntilAResult(h), 7U);

    h.engine.seek(5);
    h.engine.seekBy(-10);
    EXPECT_EQ(pumpUntilAResult(h), 0U);

    h.engine.seekBy(+100);
    EXPECT_EQ(pumpUntilAResult(h), 14U);
}

// A recording is one unbroken run, as with a restart, and the status says why it stopped.
TEST(Engine, SeekStopsTheRecording) {
    const test_support::ScratchFolder folder;
    Harness h;
    h.engine.startReplay(openerOf(framesApart(10, 40ms)));
    h.engine.startRecording(recorderIn(folder.path()));
    pausedAt(h, 2);
    ASSERT_TRUE(h.engine.status().recording);

    h.engine.seek(7);
    EXPECT_EQ(h.nextNumber(), 7U);

    EXPECT_FALSE(h.engine.status().recording);
    EXPECT_EQ(h.engine.status().message, "recording stopped: the replay moved");
}

// A video's reading thread may take a while to find the frame: the status says so until it is shown.
TEST(Engine, SeekingUntilTheFrameArrives) {
    Harness h;
    bool ready = true;
    h.engine.startReplay([&ready](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        auto source = std::make_unique<ListSource>(framesApart(10, 40ms), true, nullptr);
        source->readyWhen = [&ready] { return ready; };
        return source;
    });
    pausedAt(h, 2);
    EXPECT_FALSE(h.engine.status().seeking);

    ready = false;
    h.engine.seek(7);
    EXPECT_EQ(h.nextNumber(), std::nullopt);
    EXPECT_TRUE(h.engine.status().seeking);
    EXPECT_EQ(h.engine.status().seekTarget, std::optional<std::uint64_t>(7));

    ready = true;
    EXPECT_EQ(h.nextNumber(), 7U);
    EXPECT_FALSE(h.engine.status().seeking);
    EXPECT_EQ(h.engine.status().seekTarget, std::nullopt);
}

// A count worked out from a duration can be a frame too many: a seek to the last then finds the end.
class OverstatedSource : public ListSource {
public:
    using ListSource::ListSource;
    std::optional<std::uint64_t> frameCount() const override { return *ListSource::frameCount() + 1; }
};

TEST(Engine, SeekingClearsAtTheEnd) {
    Harness h;
    h.engine.startReplay([](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        return std::make_unique<OverstatedSource>(framesApart(5, 40ms), true, nullptr);
    });
    pausedAt(h, 1);

    h.engine.seek(100);
    h.engine.pump();
    h.engine.pump();

    EXPECT_EQ(h.engine.status().state, EngineState::Ended);
    EXPECT_FALSE(h.engine.status().seeking);
    EXPECT_EQ(h.engine.status().seekTarget, std::nullopt);
}

// A seek asked for before another source was opened is for the old one: the new one starts at its first
// frame.
TEST(Engine, PendingSeekDiesWithTheSource) {
    Harness h;
    ListSource::Counters first;
    ListSource::Counters second;
    h.engine.startReplay(openerOf(framesApart(10, 40ms), true, &first));
    pausedAt(h, 4);

    h.engine.seekBy(-1);
    h.engine.startReplay(openerOf(framesApart(10, 40ms), true, &second));

    EXPECT_EQ(pumpUntilAResult(h), 0U);
    EXPECT_EQ(first.seeks, 0);
    EXPECT_EQ(second.seeks, 0);
}

TEST(Engine, StatusHasRateAndSeek) {
    Harness h;
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    h.engine.pump();
    EXPECT_EQ(h.engine.status().frameRate, std::optional<double>(30.0));
    EXPECT_TRUE(h.engine.status().canSeek);

    h.engine.startReplay(openerOf(framesApart(3, 40ms), false));
    h.engine.pump();
    EXPECT_FALSE(h.engine.status().canSeek);

    h.engine.startLive("camera");
    h.engine.pump();
    EXPECT_EQ(h.engine.status().frameRate, std::nullopt);
    EXPECT_FALSE(h.engine.status().canSeek);
}

// An image file is processed once; Step and Restart process it again, Play does nothing more, Loop stays off.
TEST(Engine, OneFrameSource) {
    Harness h;
    CountingProcessor::Calls calls;
    h.engine.setProcessor(std::make_unique<CountingProcessor>(calls));
    h.engine.setLoop(true);
    h.engine.startReplay(openerOf(framesApart(1, 0ms)));

    EXPECT_EQ(h.nextNumber(), 0U);
    h.engine.pump();
    EXPECT_EQ(h.engine.status().state, EngineState::Ended);
    EXPECT_FALSE(h.engine.status().loop);

    h.engine.play();
    h.now += 1s;
    h.engine.pump();
    h.engine.pump();
    EXPECT_EQ(calls.processed.size(), 1U);

    h.engine.step();
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(calls.processed.size(), 2U);

    h.engine.restart();
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(calls.processed.size(), 3U);
}

// A reader's error stops the replay where it happened, never as its end.
TEST(Engine, ReaderErrorStopsWithItsFrame) {
    Harness h;
    std::vector<ReadResult> reads = framesApart(2, 40ms);
    reads.push_back({.status = ReadStatus::Error, .frame = {}, .error = "bad data"});
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(std::move(reads)));
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(h.nextNumber(), 1U);
    const int changes = h.statusChanges;

    EXPECT_TRUE(h.engine.pump());

    EXPECT_EQ(h.engine.status().state, EngineState::Stopped);
    EXPECT_EQ(h.engine.status().message, "stopped at frame 3: bad data");
    EXPECT_GT(h.statusChanges, changes);
}

TEST(Engine, SourceThatCannotOpenStops) {
    Harness h;
    h.engine.startReplay([](std::string& error) -> std::unique_ptr<frame_io::ReplaySource> {
        error = "clip.mov: cannot read it";
        return nullptr;
    });

    h.engine.pump();

    EXPECT_EQ(h.engine.status().state, EngineState::Stopped);
    EXPECT_EQ(h.engine.status().message, "clip.mov: cannot read it");
}

// --- The processor
// ------------------------------------------------------------------------------------------

TEST(Engine, ProcessorThatThrowsIsCounted) {
    Harness h;
    CountingProcessor::Calls calls;
    auto processor = std::make_unique<CountingProcessor>(calls);
    processor->throwAt = 1;
    h.engine.setProcessor(std::move(processor));
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    EXPECT_EQ(h.nextNumber(), 0U);

    const auto failed = h.next();

    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->frame.number, 1U);
    EXPECT_EQ(failed->processorError, "processor failed");
    EXPECT_TRUE(failed->processed.findings.empty());
    EXPECT_EQ(h.engine.status().processorErrors, 1U);
    EXPECT_EQ(h.nextNumber(), 2U); // processing goes on
}

TEST(Engine, ProcessorThatChangesItsFrameIsReported) {
#if defined(NDEBUG)
    GTEST_SKIP() << "the frame is checked in debug and sanitizer builds only";
#else
    Harness h;
    CountingProcessor::Calls calls;
    auto processor = std::make_unique<CountingProcessor>(calls);
    processor->writeInput = true;
    h.engine.setProcessor(std::move(processor));
    h.engine.startReplay(openerOf(framesApart(1, 0ms)));

    const auto result = h.next();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->processorError, "the processor changed its input frame");
    EXPECT_EQ(h.engine.status().processorErrors, 1U);
#endif
}

// Another processor applies to the frame on screen at once, so a paused replay shows its results.
TEST(Engine, ProcessorChangeRedoesTheFrameOnScreen) {
    Harness h;
    CountingProcessor::Calls first;
    CountingProcessor::Calls second;
    h.engine.setProcessor(std::make_unique<CountingProcessor>(first, "first"));
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    EXPECT_EQ(h.nextNumber(), 0U);
    h.engine.pause();
    h.engine.pump();

    h.engine.setProcessor(std::make_unique<CountingProcessor>(second, "second"));
    const auto result = h.next();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->frame.number, 0U);
    EXPECT_EQ(result->processed.findings, std::vector<std::string>{"second"});
    EXPECT_EQ(second.resets, 1);
    EXPECT_EQ(second.processed, std::vector<std::uint64_t>{0});
}

// --- Results
// ------------------------------------------------------------------------------------------------

// A new run clears the slot and drops what the old one left waiting, even a frame offered before it started.
TEST(Engine, StaleGenerationNeverReachesTheSlot) {
    Harness h;
    h.engine.startLive("First camera");
    h.engine.offer(pending(0, 0ms));
    h.engine.pump(); // its result waits in the slot, not taken
    const std::uint64_t generation = h.engine.status().generation;
    h.engine.offer(pending(1, 33ms)); // the old camera's, still waiting

    h.engine.startLive("Second camera");
    EXPECT_EQ(h.next(), std::nullopt);

    h.engine.offer(pending(0, 0ms));
    const auto result = h.next();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->generation, generation + 1);
    EXPECT_EQ(result->frame.number, 0U);
}

// A result replaced before the window took it is counted; the window hears of the first only.
TEST(Engine, UntakenResultsAreCounted) {
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));

    h.engine.pump();
    h.engine.pump();
    h.engine.pump();

    EXPECT_EQ(h.engine.status().resultsNotTaken, 2U);
    EXPECT_EQ(h.resultsReady, 1);
    const auto result = h.engine.takeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->frame.number, 2U);
}

// --- Recording
// ----------------------------------------------------------------------------------------------

// A recording is one unbroken run: a restart stops it, and it keeps the frames before.
TEST(Engine, RecordingStopsAtABreak) {
    const test_support::ScratchFolder folder;
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(5, 40ms)));
    h.engine.startRecording(recorderIn(folder.path()));
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(h.nextNumber(), 1U);
    EXPECT_TRUE(h.engine.status().recording);

    h.engine.restart();
    EXPECT_EQ(h.nextNumber(), 0U);

    EXPECT_FALSE(h.engine.status().recording);
    ASSERT_TRUE(h.engine.status().recordingProgress);
    EXPECT_TRUE(h.engine.status().recordingProgress->finished);
    EXPECT_EQ(h.engine.status().recordingProgress->written, 2U);
    EXPECT_EQ(framesRecordedIn(folder.path()), 2U);
}

// A replay waits for room in the recorder rather than leave a frame out.
TEST(Engine, ReplayWaitsForTheRecorder) {
    const test_support::ScratchFolder folder;
    Harness h;
    RecordingWriter::Options options = immediateRecorder();
    options.queueBytes = kFrameBytes; // one frame
    auto writer = recorderIn(folder.path(), options);
    RecordingWriter* recorder = writer.get();
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(4, 40ms)));
    h.engine.startRecording(std::move(writer));
    EXPECT_EQ(h.nextNumber(), 0U); // queued: the queue is full
    EXPECT_EQ(h.nextNumber(), 1U); // shown, waiting for room

    EXPECT_FALSE(h.engine.pump());
    EXPECT_FALSE(h.engine.pump());
    EXPECT_EQ(h.engine.takeResult(), std::nullopt);

    recorder->pump(); // frame 0 written
    EXPECT_EQ(h.nextNumber(), 2U);
    recorder->pump();
    h.engine.stopRecording();
    h.engine.pump();
    EXPECT_EQ(framesRecordedIn(folder.path()), 3U);
    EXPECT_EQ(h.engine.status().recordingProgress->notRecorded, 0U);
}

// A camera does not wait: a frame that finds the queue full is left out of the recording, and counted.
TEST(Engine, LiveFullQueueCountsNotRecorded) {
    const test_support::ScratchFolder folder;
    Harness h;
    RecordingWriter::Options options = immediateRecorder();
    options.queueBytes = kFrameBytes;
    h.engine.startLive("Test camera");
    h.engine.startRecording(recorderIn(folder.path(), options));
    h.engine.offer(pending(0, 0ms));
    EXPECT_EQ(h.nextNumber(), 0U);

    h.engine.offer(pending(1, 33ms));
    EXPECT_EQ(h.nextNumber(), 1U);

    ASSERT_TRUE(h.engine.status().recordingProgress);
    EXPECT_EQ(h.engine.status().recordingProgress->notRecorded, 1U);
    EXPECT_TRUE(h.engine.status().recording);
}

TEST(Engine, LowDiskStopsTheRecording) {
    const test_support::ScratchFolder folder;
    Harness h;
    bool lowDisk = false;
    RecordingWriter::Options options = immediateRecorder();
    options.freeSpace = [&lowDisk](const std::filesystem::path&) {
        return lowDisk ? std::uintmax_t{1'000'000} : std::uintmax_t{100'000'000'000};
    };
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    h.engine.startRecording(recorderIn(folder.path(), options));
    EXPECT_EQ(h.nextNumber(), 0U);
    const int changes = h.statusChanges;

    lowDisk = true;
    EXPECT_EQ(h.nextNumber(), 1U); // the replay goes on

    EXPECT_FALSE(h.engine.status().recording);
    EXPECT_EQ(h.engine.status().message, "recording stopped: low disk space");
    EXPECT_GT(h.statusChanges, changes);
    EXPECT_EQ(framesRecordedIn(folder.path()), 1U);
}

// recording.json's counts are those of the recording, not of the whole run before it.
TEST(Engine, RecordingCountsAreTheRecordings) {
    const test_support::ScratchFolder folder;
    Harness h;
    h.engine.setSender(std::make_unique<lab_core::ResultSender>(
        lab_core::ResultSender::Mode::Immediate, std::make_unique<test_support::RecordingSender>()));
    h.engine.startLive("Test camera");
    h.engine.offer(pending(0, 0ms));
    h.engine.offer(pending(1, 33ms)); // dropped
    h.engine.pump();                  // the sender holds frame 1, the slot too
    h.engine.offer(pending(2, 66ms));
    h.engine.pump(); // frame 2 replaces 1 in the sender and in the slot
    h.engine.reportNotDrawn(3);

    h.engine.startRecording(recorderIn(folder.path()));
    h.engine.offer(pending(3, 99ms));
    h.engine.offer(pending(4, 132ms)); // dropped
    h.engine.pump();                   // frame 4 replaces 2 in the sender and in the slot
    h.engine.reportNotDrawn(7);
    h.engine.stopRecording();
    h.engine.pump();

    std::ifstream stream(folder.path() / "recording.json");
    const nlohmann::json json = nlohmann::json::parse(stream);
    EXPECT_EQ(json["counts"]["droppedBeforeProcessing"], 1);
    EXPECT_EQ(json["counts"]["notDrawn"], 4 + 1); // reported, and one result not taken
    EXPECT_EQ(json["counts"]["notSent"], 1);
    EXPECT_EQ(json["counts"]["notRecorded"], 0);
}

// Stopped, with the recording written and nothing running, the engine can be destroyed at once.
TEST(Engine, ShutDownOnceStoppedAndWritten) {
    const test_support::ScratchFolder folder;
    Harness h;
    EXPECT_TRUE(h.engine.shutDown());
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    h.engine.startRecording(recorderIn(folder.path()));
    h.engine.pump();
    EXPECT_FALSE(h.engine.shutDown());

    h.engine.stop();
    EXPECT_FALSE(h.engine.shutDown()); // the command still waits
    h.engine.pump();

    EXPECT_TRUE(h.engine.shutDown());
    EXPECT_EQ(h.engine.status().state, EngineState::Stopped);
}

// --- Sending
// ------------------------------------------------------------------------------------------------

// An ImageSender that says when it is destroyed.
class NoticedSender : public test_support::RecordingSender {
public:
    NoticedSender(test_support::Latch* holdLive, std::atomic<bool>& gone)
        : RecordingSender([](std::string_view) { return visual_debug::SendResult::Delivered; }, holdLive),
          m_gone(gone) {}
    ~NoticedSender() override { m_gone = true; }

private:
    std::atomic<bool>& m_gone;
};

// A sender stuck on a slow viewer is retired without the engine waiting for it.
TEST(Engine, ReplacingTheSenderDoesNotWait) {
    test_support::Latch latch;
    std::atomic<bool> gone = false;
    {
        Harness h;
        h.engine.setSender(std::make_unique<lab_core::ResultSender>(
            lab_core::ResultSender::Mode::Threaded, std::make_unique<NoticedSender>(&latch, gone)));
        h.engine.startLive("Test camera");
        h.engine.offer(pending(0, 0ms));
        h.engine.pump();
        ASSERT_TRUE(latch.waitUntilEntered()); // held in sendLive

        h.engine.setSender(std::make_unique<lab_core::ResultSender>(
            lab_core::ResultSender::Mode::Immediate, std::make_unique<test_support::RecordingSender>()));
        h.engine.pump(); // returns, the old sender still held

        EXPECT_FALSE(gone);
        latch.release();
    }
    EXPECT_TRUE(gone);
}

// --- The processing thread
// ----------------------------------------------------------------------------------

// What the hooks report, waited for on the test's thread.
class Signals {
public:
    void notify() {
        const std::lock_guard lock(m_mutex);
        ++m_count;
        m_changed.notify_all();
    }
    bool waitFor(const std::function<bool()>& condition) {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return condition(); });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_count = 0;
};

// A command wakes the processing thread from a long wait for the next frame's time.
TEST(Engine, ThreadedCommandWakesAWait) {
    Signals signals;
    std::vector<ReadResult> reads = framesApart(1, 0ms);
    reads.push_back({.status = ReadStatus::Frame, .frame = frameOf(1, 1h), .error = {}});
    Engine engine(Engine::Mode::Threaded, {.resultReady = [&] { signals.notify(); },
                                           .statusChanged = [&] { signals.notify(); },
                                           .now = nullptr});
    engine.startReplay(openerOf(std::move(reads)));
    ASSERT_TRUE(signals.waitFor([&] { return engine.status().frameNumber == 0U; }));

    engine.stop();

    EXPECT_TRUE(signals.waitFor([&] { return engine.status().state == EngineState::Stopped; }));
}

// The source opens on the processing thread, so a slow disk never holds up the window.
TEST(Engine, OpensOnTheProcessingThread) {
    Signals signals;
    std::mutex mutex;
    std::optional<std::thread::id> openedOn;
    Engine engine(Engine::Mode::Threaded, {.resultReady = nullptr, .statusChanged = nullptr, .now = nullptr});

    engine.startReplay([&](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        {
            const std::lock_guard lock(mutex);
            openedOn = std::this_thread::get_id();
        }
        signals.notify();
        return std::make_unique<ListSource>(framesApart(1, 0ms), true, nullptr);
    });

    ASSERT_TRUE(signals.waitFor([&] {
        const std::lock_guard lock(mutex);
        return openedOn.has_value();
    }));
    const std::lock_guard lock(mutex);
    EXPECT_NE(*openedOn, std::this_thread::get_id());
}

} // namespace
