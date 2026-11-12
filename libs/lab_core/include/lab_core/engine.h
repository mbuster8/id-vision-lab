#pragma once

#include "lab_core/focus_control.h"
#include "lab_core/frame_processor.h"
#include "lab_core/lab_result.h"
#include "lab_core/result_sender.h"

#include <frame_io/frame.h>
#include <frame_io/recording.h>
#include <frame_io/replay_pacer.h>
#include <frame_io/replay_source.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lab_core {

using Clock = std::chrono::steady_clock;

class FocusController;

// A camera frame not yet converted: the conversion runs on the processing thread, so the camera's thread only
// hands over its buffer. Nothing, with `error`, when the frame cannot be converted.
struct PendingFrame {
    std::function<std::optional<frame_io::Frame>(std::string& error)> convert;
};

// Opens a replay's source; runs on the processing thread, so a slow disk never holds up the window. Null,
// with `error`, when it cannot.
using ReplayOpener = std::function<std::unique_ptr<frame_io::ReplaySource>(std::string& error)>;

enum class EngineState { Idle, Live, Playing, Paused, Ended, Stopped };

struct EngineStatus {
    EngineState state = EngineState::Idle;
    std::uint64_t generation = 0; // the current run's: a new source, a restart, a seek or a loop
    std::string source;           // the camera's name, or the file's or the folder's
    std::optional<std::uint64_t> frameNumber;
    std::optional<std::uint64_t> frameCount;
    std::optional<double> frameRate; // a replay's, to turn frame numbers into times
    bool canSeek = false;            // a replay that can go to any frame
    bool canStepBack = false;
    bool seeking = false;                    // a seek's frame is not shown yet
    std::optional<std::uint64_t> seekTarget; // that frame
    bool loop = false;
    bool asFastAsPossible = false;
    std::uint64_t droppedBeforeProcessing = 0; // camera frames replaced by a newer one before processing
    std::uint64_t resultsNotTaken = 0;         // results replaced by a newer one before the window took them
    std::uint64_t processorErrors = 0;
    std::string message;                    // "stopped at frame 3: …", "recording stopped: low disk space", …
    std::optional<std::uint64_t> busyFrame; // process() running on it since busySince
    std::optional<Clock::time_point> busySince;
    bool recording = false;
    // The recording's, or once stopped the last one's, also while it finishes writing its queue.
    std::optional<frame_io::RecordingWriter::Progress> recordingProgress;
    std::optional<ResultSender::Stats> sending; // while sending
    bool endingChannels = false;                // a stopped sender is still ending the lab's channels
};

struct EngineHooks {
    std::function<void()> resultReady;      // a result entered the empty slot
    std::function<void()> statusChanged;    // the state, the message or the recording changed
    std::function<Clock::time_point()> now; // null: Clock::now
};

// Moves frames from a source through the processor to the window, the recorder and the sender, on one
// processing thread. The window posts commands, which the thread runs between frames, and never waits for it.
// A camera offers its frames, the newest replacing one still waiting; a replay is read in order, every frame,
// at its own pace or slower. Each source run has a generation: a new source, a restart, a step back and a
// loop or a seek start one, and nothing of an older one reaches the slot, the recorder or the sender. Seeks
// asked for faster than they are done add up to one. Hooks are called
// on the processing thread (in Immediate mode, in pump()) with no lock held.
class Engine {
public:
    // Threaded: a processing thread of its own. Immediate: pump() does the work, for the tests.
    enum class Mode { Threaded, Immediate };

    Engine(Mode mode, EngineHooks hooks);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    // Stops, waits for process() to return, writes the recording's queue, retires the sender, and joins.
    ~Engine();

    // A camera run, named for the status line; its frames come through offer(). `focus`: the camera's own
    // focus control, opened and used on the processing thread only, and destroyed there when the run ends;
    // null when the lab cannot set this camera's focus. The run's FocusController puts the camera's focus
    // back.
    void startLive(std::string name, std::unique_ptr<FocusControl> focus = nullptr);
    // A click at (x, y), shares of the frame: the camera run's focus searches there. A newer one replaces one
    // still waiting; outside a camera run, nothing.
    void focusAt(double x, double y);
    // A frame of the camera run; one still waiting is replaced and counted, and destroyed without the lock.
    void offer(PendingFrame frame);
    // A replay, opened on the processing thread, then playing from frame 0.
    void startReplay(ReplayOpener open);
    void stop();
    void play();
    void pause();
    // Paused: the next frame. Playing: pauses there. An image file: processes it again.
    void step();
    // A replay that can seek: frame `index` (the last if past it), as a new generation; playing on when
    // playing, else paused showing it. It replaces a seek or steps still waiting.
    void seek(std::uint64_t index);
    // `frames` on (back when negative) from the frame on screen, or from a seek still running or waiting,
    // within the first and the last frame. Added to steps still waiting, so holding ← makes one seek, not
    // ten.
    void seekBy(std::int64_t frames);
    // seekBy(-1).
    void stepBack();
    // Back to frame 0, as a new generation.
    void restart();
    void setLoop(bool on);
    void setAsFastAsPossible(bool on);
    // Null is None. The processor is reset() and, unless frames are coming, the frame on screen processed
    // again.
    void setProcessor(std::unique_ptr<FrameProcessor> processor);
    // The processor's reset() and nothing else (A4's Scan again, Discard and Scan another): no new
    // generation, so a recording in progress and a camera run's focus go on.
    void resetProcessor();
    // Records the frames processed from now on. A restart, a seek, a loop, another source and stop() also
    // stop the recording; its counts are those since this call.
    void startRecording(std::unique_ptr<frame_io::RecordingWriter> writer);
    // The recording's queue is written in the background; its progress shows in the status until finished.
    void stopRecording();
    // Null: not sending. A replaced sender is destroyed on a thread of its own, so a send in progress to a
    // slow viewer never holds up the processing or the window.
    // While nothing runs (paused, ended, one picture), a new sender gets the frame on screen at once.
    void setSender(std::unique_ptr<ResultSender> sender);
    // ⏸: the viewer keeps the lab's channels showing the last frame sent. Resumed while nothing runs, the
    // frame on screen goes out again. Without a sender, nothing.
    void pauseSending(bool paused);
    // ■: the sender is retired as setSender(nullptr) retires it; with `endChannels`, it first ends the lab's
    // channels in the viewer, and status().endingChannels is true from this call until it has.
    void stopSending(bool endChannels);
    // The canvas's replaced pictures since the window opened, for recording.json.
    void reportNotDrawn(std::uint64_t total);

    // The newest result, once.
    std::optional<LabResult> takeResult();
    EngineStatus status() const;
    // Neither running nor processing, no command waiting, every recording written, every retired sender gone:
    // the engine can be destroyed at once. The window calls setSender(nullptr) first, so a send in progress
    // is waited for here rather than in the destructor.
    bool shutDown() const;
    // Immediate mode: the commands waiting, then at most one frame. Whether anything was done.
    bool pump();

private:
    struct Pending {
        PendingFrame frame;
        std::uint64_t number = 0;
        std::uint64_t run = 0;
    };
    // A sender retired on a thread of its own, ending its channels first or not.
    struct Retired {
        std::future<void> done;
        bool endingChannels = false;
    };
    struct Totals {
        std::uint64_t dropped = 0;
        std::uint64_t notTaken = 0;
        std::uint64_t notDrawn = 0;
        std::uint64_t notSent = 0;
    };
    // The seek the window asked for and the processing thread has not done yet: an absolute frame, steps from
    // it (or from the frame on screen), or both. Its number is the one its doSeek command carries: a request
    // made after another source, a stop or a restart is posted behind that command, so an older doSeek still
    // in the mailbox ahead of it must leave it alone.
    struct SeekRequest {
        std::optional<std::uint64_t> absolute;
        std::int64_t relative = 0;
        std::uint64_t number = 0;
    };
    // What one turn of the processing loop found to wait for.
    struct Turn {
        bool did = false;
        std::optional<Clock::time_point> wakeAt; // the next frame's due time
        bool waitForRecorder = false;
    };

    void post(std::function<void()> command);
    void signal();
    void run();
    Turn turn();
    void frameWork(Turn& turn);
    void liveWork(Turn& turn);
    void replayWork(Turn& turn);
    void process(frame_io::Frame frame, bool record);
    void record(const frame_io::Frame& frame);
    bool pushWaitingFrame();
    void notify();
    void pruneFinished();
    void closeDown();

    // Commands, on the processing thread.
    void doStartLive(const std::string& name, std::uint64_t run, std::unique_ptr<FocusControl> focus);
    void doFocusAt(std::uint64_t run);
    // Ends the camera run's focus: puts the camera's focus back, then destroys the controller and its
    // control. A failed restore, at the end or during the run, is kept in m_restoreFailure.
    void endFocusRun();
    void doStartReplay(const ReplayOpener& open);
    void doStop();
    void doPlay();
    void doPause();
    void doStep();
    void doSeek(std::uint64_t number);
    void doRestart();
    void doSetLoop(bool on);
    void doSetAsFastAsPossible(bool on);
    void doSetProcessor(std::unique_ptr<FrameProcessor> processor);
    void doStartRecording(std::unique_ptr<frame_io::RecordingWriter> writer, Totals base);
    void doSetSender(std::unique_ptr<ResultSender> sender);
    void doPauseSending(bool paused);
    void doStopSending(bool endChannels);
    void retireSender(bool endChannels);

    void newGeneration();
    void clearSeeking();
    void stopFailedRecording();
    void stopAfterThrow(const std::string& what);
    void stopRecordingNow(const std::string& message = {});
    void stopWithError(const std::string& message);
    void setState(EngineState state);
    void setMessage(std::string message);
    void resetProcessorNow();
    void closeSource();
    Totals totals() const; // with the lock
    Clock::time_point now() const;

    const Mode m_mode;
    EngineHooks m_hooks;

    // The processing thread's own.
    std::unique_ptr<FrameProcessor> m_processor;
    std::unique_ptr<frame_io::ReplaySource> m_source;
    bool m_live = false;
    std::uint64_t m_liveRun = 0; // the camera run whose frames are taken
    EngineState m_state = EngineState::Idle;
    std::uint64_t m_generation = 0;
    frame_io::ReplayPacer m_pacer;
    bool m_loop = false;
    bool m_asFast = false;
    bool m_oneFrame = false;
    bool m_stepWanted = false;
    std::uint64_t m_nextIndex = 0;             // the number of the next frame the source will read
    std::optional<std::uint64_t> m_seekTarget; // the frame a seek went to, until it is shown
    std::optional<frame_io::Frame> m_held;     // read and not yet due
    std::optional<Clock::time_point> m_heldDue;
    std::optional<frame_io::Frame> m_onScreen; // the frame processed last
    std::optional<LabResult> m_lastResult;     // its result, until the source closes
    std::optional<frame_io::Frame>
        m_waitingToRecord;                    // a replay's frame the full recorder has no room for yet
    std::unique_ptr<FocusController> m_focus; // a camera run's
    bool m_restoreFailedReported = false;
    // A camera's focus could not be put back: added to every message until a later restore succeeds, so the
    // stop and start that the window sends together never hide it.
    std::string m_restoreFailure;
    std::string m_messageBase; // the message as set, before the restore failure is added
    Totals m_recordingBase;
    bool m_notifyStatus = false;
    bool m_notifyResult = false;

    // Under the lock. The processing thread changes m_recorder and m_sender only with it held, and uses them
    // without it.
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_signalled = false;
    bool m_quit = false;
    std::deque<std::function<void()>> m_commands;
    std::optional<Pending> m_pending;
    std::optional<SeekRequest> m_seekRequest; // a doSeek command is in the mailbox while there is one
    std::uint64_t m_seekRequests = 0;         // seek requests made, which numbers them
    std::optional<std::pair<double, double>> m_focusRequest; // the newest click, for doFocusAt
    bool m_acceptLive = false;
    std::uint64_t m_offered = 0;
    std::uint64_t m_run = 0; // camera runs started, and stopped
    EngineStatus m_status;
    std::optional<LabResult> m_slot;
    std::uint64_t m_notDrawn = 0;
    std::uint64_t m_notSentRetired = 0;
    std::unique_ptr<frame_io::RecordingWriter> m_recorder;
    std::vector<std::unique_ptr<frame_io::RecordingWriter>> m_finishing;
    std::optional<frame_io::RecordingWriter::Progress> m_lastRecording;
    std::unique_ptr<ResultSender> m_sender;
    mutable std::vector<Retired> m_retiring;
    int m_endsAsked = 0; // stopSending(true) calls whose command has not run yet

    std::thread m_thread; // last, so it starts after everything it uses
};

} // namespace lab_core
