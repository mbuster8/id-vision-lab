#include "lab_core/document_processor.h"
#include "lab_core/engine.h"
#include "lab_core/focus_controller.h"

#include "fake_focus_control.h"
#include "fake_mrz_reader.h"
#include "fake_sender.h"
#include "latch.h"
#include "list_source.h"
#include "scratch_folder.h"
#include "scripted_finder.h"

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
        return {.overlays = {}, .images = {}, .findings = {lab_core::Finding{m_name, ""}}};
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

// A click on the slider seeks on the press and again on the release, and holding ← at the first frame keeps
// asking for frame 0: a seek already running to that frame goes on rather than start again.
TEST(Engine, SameTargetDoesNotRestartTheSeek) {
    Harness h;
    ListSource::Counters counters;
    bool ready = true;
    h.engine.startReplay([&](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        auto source = std::make_unique<ListSource>(framesApart(10, 40ms), true, &counters);
        source->readyWhen = [&ready] { return ready; };
        return source;
    });
    pausedAt(h, 2);
    ready = false;

    h.engine.seek(7);
    h.engine.pump();
    h.engine.seek(7);
    h.engine.pump();
    EXPECT_EQ(counters.seeks, 1);

    h.engine.seek(0);
    h.engine.pump();
    h.engine.seekBy(-1);
    h.engine.pump();
    EXPECT_EQ(counters.seeks, 2);

    ready = true;
    EXPECT_EQ(pumpUntilAResult(h), 0U);
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

// A step asked for once another source is opening is for the new one, though the command of a step asked for
// before it is still in the mailbox, ahead of the opening.
TEST(Engine, StepAfterAnotherSourceIsForIt) {
    Harness h;
    ListSource::Counters first;
    ListSource::Counters second;
    h.engine.startReplay(openerOf(framesApart(10, 40ms), true, &first));
    pausedAt(h, 4);

    h.engine.seekBy(-1);
    h.engine.startReplay(openerOf(framesApart(10, 40ms), true, &second));
    h.engine.seekBy(+3);

    EXPECT_EQ(pumpUntilAResult(h), 3U);
    EXPECT_EQ(first.seeks, 0);
    EXPECT_EQ(second.seeks, 1);
}

// The same with a restart: a seek asked for after it goes to its frame once the restart has run.
TEST(Engine, SeekAfterARestartIsForIt) {
    Harness h;
    ListSource::Counters counters;
    h.engine.startReplay(openerOf(framesApart(10, 40ms), true, &counters));
    pausedAt(h, 4);

    h.engine.seekBy(-1);
    h.engine.restart();
    h.engine.seek(7);

    EXPECT_EQ(pumpUntilAResult(h), 7U);
    EXPECT_EQ(counters.lastSeek, std::optional<std::uint64_t>(7));
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

// Whatever a source throws stops the run with the reason; it never leaves the processing thread, which would
// end the program.
TEST(Engine, ThrowingSourceStopsTheRun) {
    Harness h;
    h.engine.setAsFastAsPossible(true);
    h.engine.startReplay([](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        auto source = std::make_unique<ListSource>(framesApart(5, 40ms), true, nullptr);
        source->throwAtNext = 2;
        return source;
    });
    EXPECT_EQ(h.nextNumber(), 0U);
    EXPECT_EQ(h.nextNumber(), 1U);

    h.engine.pump();

    EXPECT_EQ(h.engine.status().state, EngineState::Stopped);
    EXPECT_NE(h.engine.status().message.find("internal error (disk gone)"), std::string::npos)
        << h.engine.status().message;
    EXPECT_FALSE(h.engine.status().busyFrame);
    EXPECT_TRUE(h.engine.shutDown());
}

// A command that throws, here something that is not even a std::exception, stops the run too.
TEST(Engine, ThrowingCommandStopsTheRun) {
    Harness h;
    h.engine.startReplay([](std::string&) -> std::unique_ptr<frame_io::ReplaySource> { throw 42; });

    h.engine.pump();

    EXPECT_EQ(h.engine.status().state, EngineState::Stopped);
    EXPECT_NE(h.engine.status().message.find("internal error"), std::string::npos)
        << h.engine.status().message;
    EXPECT_TRUE(h.engine.shutDown());
}

// A recording whose writer failed in the background stops at once, also while nothing is pushed to it.
TEST(Engine, FailureNoticedWhilePaused) {
    const test_support::ScratchFolder folder;
    Harness h;
    std::unique_ptr<RecordingWriter> recorder = recorderIn(folder.path());
    RecordingWriter* writer = recorder.get();
    h.engine.startReplay(openerOf(framesApart(5, 40ms)));
    h.engine.startRecording(std::move(recorder));
    pausedAt(h, 1);
    ASSERT_TRUE(h.engine.status().recording);
    std::filesystem::create_directory(folder.path() / "000001.png"); // where frame 1's file would go

    writer->pump();
    writer->pump();
    h.engine.pump();

    EXPECT_FALSE(h.engine.status().recording);
    EXPECT_EQ(h.engine.status().message.rfind("recording stopped:", 0), 0U) << h.engine.status().message;
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
    EXPECT_EQ(result->processed.findings, (std::vector<lab_core::Finding>{{"second", ""}}));
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

// Pumps, a millisecond apart, until the condition holds or ten seconds pass; whether it held.
bool pumpUntil(Harness& h, const std::function<bool()>& condition) {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        h.engine.pump();
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A sender the engine takes over, with pointers the test keeps.
struct KeptSender {
    explicit KeptSender(test_support::Latch* holdLive = nullptr, test_support::SendLog* log = nullptr,
                        lab_core::ResultSender::Mode mode = lab_core::ResultSender::Mode::Immediate) {
        auto fake = std::make_unique<test_support::RecordingSender>(
            [](std::string_view) { return visual_debug::SendResult::Delivered; }, holdLive);
        fake->log = log;
        recording = fake.get();
        owned = std::make_unique<lab_core::ResultSender>(mode, std::move(fake));
        sender = owned.get();
    }
    std::unique_ptr<lab_core::ResultSender> owned;
    lab_core::ResultSender* sender = nullptr;
    test_support::RecordingSender* recording = nullptr;
};

// The window disables ▶ from the click of ■ until the old sender has ended its channels, so a new stream's
// "lab" is never ended by the old sender's late end.
TEST(Engine, EndingChannelsFromTheCall) {
    test_support::Latch latch;
    Harness h;
    KeptSender kept(&latch, nullptr, lab_core::ResultSender::Mode::Threaded);
    h.engine.setSender(std::move(kept.owned));
    h.engine.startReplay(openerOf(framesApart(3, 40ms)));
    h.engine.pump();
    ASSERT_TRUE(latch.waitUntilEntered()); // frame 0 held in its send

    h.engine.stopSending(true);
    EXPECT_TRUE(h.engine.status().endingChannels); // before the command runs
    h.engine.pump();
    EXPECT_TRUE(h.engine.status().endingChannels); // the old sender waits for its send
    latch.release();

    EXPECT_TRUE(pumpUntil(h, [&] { return !h.engine.status().endingChannels; }));
}

// ▶ on a paused replay sends the frame on screen at once rather than nothing until the next frame.
TEST(Engine, NewSenderGetsTheLastResultWhilePaused) {
    Harness h;
    h.engine.startReplay(openerOf(framesApart(5, 40ms)));
    pausedAt(h, 2);
    KeptSender kept;
    lab_core::ResultSender* sender = kept.sender;
    test_support::RecordingSender* recording = kept.recording;

    h.engine.setSender(std::move(kept.owned));
    h.engine.pump();

    EXPECT_TRUE(sender->pump());
    const auto calls = recording->calls();
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls[0].name, "lab");
    EXPECT_EQ(calls[0].image.at<cv::Vec3b>(0, 0)[0], 2);
}

// ⏸ then ▶ while the replay is paused sends the frame on screen again.
TEST(Engine, ResumeResubmitsWhilePaused) {
    Harness h;
    KeptSender kept;
    lab_core::ResultSender* sender = kept.sender;
    test_support::RecordingSender* recording = kept.recording;
    h.engine.setSender(std::move(kept.owned));
    h.engine.startReplay(openerOf(framesApart(5, 40ms)));
    h.engine.pauseSending(true);
    pausedAt(h, 2);
    EXPECT_FALSE(sender->pump()); // nothing while paused
    EXPECT_TRUE(sender->stats().paused);

    h.engine.pauseSending(false);
    h.engine.pump();

    EXPECT_TRUE(sender->pump());
    const auto calls = recording->calls();
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back().name, "lab");
    EXPECT_EQ(calls.back().image.at<cv::Vec3b>(0, 0)[0], 2);
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

// ■ ends the lab's channels in the viewer, after its last frame.
TEST(Engine, StopSendingEndsChannels) {
    test_support::SendLog log;
    Signals signals;
    Engine engine(Engine::Mode::Threaded, {.resultReady = [&] { signals.notify(); },
                                           .statusChanged = [&] { signals.notify(); },
                                           .now = nullptr});
    KeptSender kept(nullptr, &log, lab_core::ResultSender::Mode::Threaded);
    test_support::RecordingSender* recording = kept.recording;
    engine.setSender(std::move(kept.owned));
    engine.startReplay(openerOf(framesApart(3, 0ms)));
    ASSERT_TRUE(recording->waitForCalls(1));

    engine.stopSending(true);

    ASSERT_TRUE(signals.waitFor([&] { return !engine.status().endingChannels; }));
    const std::vector<std::string> lines = log.lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(), "end lab");
}

// --- A3.3: the focus controller in camera runs
// -------------------------------------------------------------------------------------------------

std::shared_ptr<test_support::FocusLog> focusLog() { return std::make_shared<test_support::FocusLog>(); }

std::unique_ptr<lab_core::FocusControl> controlFor(const std::shared_ptr<test_support::FocusLog>& log) {
    return std::make_unique<test_support::FakeFocusControl>(log);
}

const lab_core::Finding* focusRow(const lab_core::LabResult& result) {
    for (const lab_core::Finding& finding : result.processed.findings) {
        if (finding.name == "Focus") {
            return &finding;
        }
    }
    return nullptr;
}

// A camera run whose lab has switched the camera to manual focus: a click and one frame.
void manualFocus(Harness& h, const std::shared_ptr<test_support::FocusLog>& log) {
    h.engine.startLive("camera", controlFor(log));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next());
    h.engine.focusAt(0.5, 0.5);
    h.engine.offer(pending(1, 33ms));
    ASSERT_TRUE(h.next());
    ASSERT_FALSE(log->autoFocus);
}

TEST(Engine, CameraRunCarriesFocus) {
    Harness h;
    const auto log = focusLog();
    h.engine.startLive("camera", controlFor(log));
    h.engine.offer(pending(0, 0ms));
    const auto result = h.next();
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->focus);
    EXPECT_TRUE(result->focus->controllable);
    ASSERT_FALSE(result->processed.findings.empty());
    EXPECT_EQ(result->processed.findings.back(),
              (lab_core::Finding{"Focus", "camera", lab_core::Finding::Tone::Plain}));

    h.engine.startReplay(openerOf(framesApart(1, 0ms)));
    const auto replayed = h.next();
    ASSERT_TRUE(replayed);
    EXPECT_FALSE(replayed->focus);
    EXPECT_EQ(focusRow(*replayed), nullptr);
}

TEST(Engine, NoControlStillCarriesFocus) {
    Harness h;
    h.engine.startLive("camera");
    h.engine.offer(pending(0, 0ms));
    const auto result = h.next();
    ASSERT_TRUE(result && result->focus);
    EXPECT_FALSE(result->focus->controllable);
    ASSERT_NE(focusRow(*result), nullptr);
    EXPECT_EQ(focusRow(*result)->value, "camera (lab can't set it)");
}

TEST(Engine, FocusAtIsNewestWins) {
    Harness h;
    const auto log = focusLog();
    h.engine.startLive("camera", controlFor(log));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next());
    h.engine.focusAt(0.1, 0.1);
    h.engine.focusAt(0.9, 0.9);
    h.engine.offer(pending(1, 33ms));
    const auto result = h.next();
    ASSERT_TRUE(result && result->focus);
    EXPECT_EQ(result->focus->kind, lab_core::FocusStatus::Kind::Click);
    EXPECT_EQ(log->count("auto off"), 1);
    EXPECT_EQ(log->count("has auto"), 1);
}

// Review Focus 1, end to end: a frame offered while the move was being decided never counts for it.
TEST(Engine, EngineReadsTheOfferNumberAfterTheMove) {
    Harness h;
    const auto log = focusLog();
    // Offers the camera's next frame while it processes one, as a camera does.
    class OfferingProcessor final : public lab_core::FrameProcessor {
    public:
        explicit OfferingProcessor(Engine& engine) : m_engine(engine) {}
        lab_core::ProcessorResult process(const frame_io::Frame& frame) override {
            m_engine.offer(pending(frame.number + 100, frame.at + 33ms));
            return {};
        }

    private:
        Engine& m_engine;
    };
    h.engine.setProcessor(std::make_unique<OfferingProcessor>(h.engine));
    h.engine.startLive("camera", controlFor(log));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next()); // frame 0; frame 1 offered meanwhile
    h.engine.focusAt(0.5, 0.5);
    const auto result = h.next(); // frame 1 makes the move; frame 2 is offered while it is processed
    ASSERT_TRUE(result && result->focus);
    ASSERT_EQ(result->frame.number, 1U);
    ASSERT_EQ(result->focus->state, lab_core::FocusStatus::State::Searching);
    EXPECT_EQ(result->focus->countingFrom, 3U);
}

// Review Focus 2: every end of a camera run puts the camera's focus back, and destroys its control.
TEST(Engine, EveryEndOfACameraRunRestores) {
    const auto check = [](const std::shared_ptr<test_support::FocusLog>& log, const char* how) {
        EXPECT_TRUE(log->autoFocus) << how;
        EXPECT_TRUE(log->destroyed) << how;
    };
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        h.engine.stop();
        h.engine.pump();
        check(log, "stop");
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        h.engine.startLive("another camera");
        h.engine.pump();
        check(log, "another camera");
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        h.engine.startReplay(openerOf(framesApart(1, 0ms)));
        h.engine.pump();
        check(log, "a replay");
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        h.engine.offer({[](std::string&) -> std::optional<frame_io::Frame> { throw 42; }});
        h.engine.pump();
        EXPECT_EQ(h.engine.status().state, EngineState::Stopped);
        check(log, "an internal error");
    }
    {
        const auto log = focusLog();
        {
            Harness h;
            manualFocus(h, log);
        }
        check(log, "the engine's end");
    }
}

TEST(Engine, FailedRestoreSetsTheMessage) {
    const std::string failed = lab_core::kFocusRestoreFailed;
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.stop();
        h.engine.pump();
        EXPECT_EQ(h.engine.status().message, failed) << "stop";
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.startLive("another camera");
        h.engine.pump();
        EXPECT_EQ(h.engine.status().message, failed) << "another camera";
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.startReplay(openerOf(framesApart(1, 0ms)));
        h.engine.pump();
        EXPECT_EQ(h.engine.status().message, failed) << "a replay";
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.startReplay([](std::string& error) -> std::unique_ptr<frame_io::ReplaySource> {
            error = "no such file";
            return nullptr;
        });
        h.engine.pump();
        EXPECT_EQ(h.engine.status().message, "no such file · " + failed) << "a replay that cannot open";
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.offer({[](std::string&) -> std::optional<frame_io::Frame> { throw 42; }});
        h.engine.pump();
        EXPECT_NE(h.engine.status().message.find("internal error"), std::string::npos);
        EXPECT_NE(h.engine.status().message.find(" · " + failed), std::string::npos) << "an internal error";
    }
}

TEST(Engine, FailedLetGoRestoreSetsTheMessage) {
    Harness h;
    const auto log = focusLog();
    log->failSetPosition = true; // the click's first move fails: the lab gives up and restores
    log->failAutoOn = true;      // and that restore fails
    h.engine.startLive("camera", controlFor(log));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next());
    h.engine.focusAt(0.5, 0.5);
    h.engine.offer(pending(1, 33ms));
    const auto result = h.next();
    ASSERT_TRUE(result && result->focus);
    EXPECT_TRUE(result->focus->restoreFailed);
    EXPECT_EQ(h.engine.status().state, EngineState::Live);
    EXPECT_EQ(h.engine.status().message, lab_core::kFocusRestoreFailed);
}

TEST(Engine, FocusSlotClearedByANewRun) {
    Harness h;
    const auto first = focusLog();
    h.engine.startLive("camera", controlFor(first));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next());
    h.engine.focusAt(0.5, 0.5);
    const auto second = focusLog();
    h.engine.startLive("another camera", controlFor(second));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next());
    EXPECT_EQ(first->count("auto off"), 0);
    EXPECT_EQ(second->count("auto off"), 0);
}

TEST(Engine, PendingStartDestroyedWithTheEngineMakesNoCall) {
    const auto log = focusLog();
    {
        Harness h;
        h.engine.startLive("camera", controlFor(log));
    }
    EXPECT_TRUE(log->destroyed);
    EXPECT_TRUE(log->calls.empty());
}

TEST(Engine, ControlDestroyedOnTheProcessingThread) {
    Signals signals;
    const auto log = focusLog();
    {
        Engine engine(Engine::Mode::Threaded,
                      {.resultReady = [&] { signals.notify(); }, .statusChanged = nullptr, .now = nullptr});
        engine.startLive("camera", controlFor(log));
        engine.offer(pending(0, 0ms));
        ASSERT_TRUE(signals.waitFor([&] { return engine.status().frameNumber == 0U; }));
        engine.stop();
    }
    // Read once the engine is gone: its destructor joined the processing thread.
    EXPECT_TRUE(log->destroyed);
    EXPECT_NE(log->destroyedOn, std::this_thread::get_id());
    EXPECT_EQ(log->destroyedOn, log->lastThread);
}

// Final review F1: the window always stops before it starts another camera or a replay; a failed restore's
// message must survive that start, and a failure during the run must survive the stop.
TEST(Engine, FailedRestoreSurvivesTheNextStart) {
    const std::string failed = lab_core::kFocusRestoreFailed;
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.stop();
        h.engine.startLive("another camera");
        h.engine.pump();
        EXPECT_EQ(h.engine.status().message, failed) << "stop, then another camera";
    }
    {
        Harness h;
        const auto log = focusLog();
        manualFocus(h, log);
        log->failSetAuto = true;
        h.engine.stop();
        h.engine.startReplay(openerOf(framesApart(1, 0ms)));
        h.engine.pump();
        EXPECT_EQ(h.engine.status().message, failed) << "stop, then a replay";
    }
}

TEST(Engine, FailedLetGoRestoreSurvivesTheStop) {
    Harness h;
    const auto log = focusLog();
    log->failSetPosition = true;
    log->failAutoOn = true;
    h.engine.startLive("camera", controlFor(log));
    h.engine.offer(pending(0, 0ms));
    ASSERT_TRUE(h.next());
    h.engine.focusAt(0.5, 0.5);
    h.engine.offer(pending(1, 33ms));
    ASSERT_TRUE(h.next());
    h.engine.stop();
    h.engine.pump();
    EXPECT_EQ(h.engine.status().message, lab_core::kFocusRestoreFailed);
}

// --- A4: Scan again, Discard and Scan another
// ---------------------------------------------------------------------------------------------------

// The processor starts reading afresh; the run, its recording and its generation go on.
TEST(Engine, ResetProcessorKeepsTheRun) {
    const test_support::ScratchFolder folder;
    Harness h;
    CountingProcessor::Calls calls;
    h.engine.setProcessor(std::make_unique<CountingProcessor>(calls));
    h.engine.startReplay(openerOf(framesApart(10, 40ms)));
    h.engine.startRecording(recorderIn(folder.path()));
    pausedAt(h, 2);
    const int resets = calls.resets;
    const std::uint64_t generation = h.engine.status().generation;

    h.engine.resetProcessor();
    h.engine.pump();

    EXPECT_EQ(calls.resets, resets + 1);
    EXPECT_EQ(h.engine.status().generation, generation);
    EXPECT_TRUE(h.engine.status().recording);
    EXPECT_EQ(h.engine.status().state, EngineState::Paused);
}

// The plan review: a processor swap while its MRZ reader is busy cancels the read, so the next command is
// served at once rather than after a whole read.
TEST(Engine, ProcessorSwapDuringASlowReadServesTheNextCommand) {
    auto finder = std::make_shared<test_support::Script>();
    finder->steps = {test_support::page(test_support::kPageQuad)};
    auto reader = std::make_shared<test_support::ReaderScript>();
    reader->blockReads = true;
    Engine engine(Engine::Mode::Threaded, {.resultReady = nullptr, .statusChanged = nullptr, .now = nullptr});
    engine.setProcessor(std::make_unique<lab_core::DocumentProcessor>(
        test_support::scripted(finder), doc_vision::Thresholds{},
        lab_core::DocumentProcessor::Reader{.make = test_support::scriptedReader(reader),
                                            .mode = lab_core::MrzWorker::Mode::Threaded}));
    engine.startLive("Test camera");
    const cv::Mat good = test_support::goodFrame();
    for (int n = 0; n < 200; ++n) {
        engine.offer({[good, n](std::string&) -> std::optional<frame_io::Frame> {
            return test_support::frameAt(good, n);
        }});
        if (reader->waitFor([&] { return reader->readStarted; })) {
            break;
        }
    }
    ASSERT_TRUE(reader->readStarted);

    engine.setProcessor(nullptr);
    engine.stop();
    const auto until = Clock::now() + 2s;
    while (engine.status().state != EngineState::Stopped && Clock::now() < until) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(engine.status().state, EngineState::Stopped);
    EXPECT_GE(reader->cancels, 1);
}

} // namespace
