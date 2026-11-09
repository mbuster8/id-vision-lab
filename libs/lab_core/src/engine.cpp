#include "lab_core/engine.h"

#include "lab_core/processors.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

namespace lab_core {

namespace {

// The longest the processing thread waits without looking at its mailbox while a frame's time or room in the
// recorder is coming; with nothing to wait for, a signal wakes it, and this is only a safety net.
constexpr std::chrono::milliseconds kWaitSlice{50};
constexpr std::chrono::milliseconds kIdleWait{1000};

constexpr char kLowDisk[] = "recording stopped: low disk space";
constexpr char kReplayMoved[] = "recording stopped: the replay moved";
const std::string kRecordingStopped = "recording stopped: ";

bool isRunning(EngineState state) { return state == EngineState::Live || state == EngineState::Playing; }

std::size_t bytesOf(const frame_io::Frame& frame) { return frame.image.total() * frame.image.elemSize(); }

std::uint64_t since(std::uint64_t now, std::uint64_t then) { return now > then ? now - then : 0; }

#if !defined(NDEBUG)
bool samePixels(const cv::Mat& before, const cv::Mat& after) {
    if (before.empty() || after.empty()) {
        return before.empty() == after.empty();
    }
    if (before.size() != after.size() || before.type() != after.type()) {
        return false;
    }
    return cv::norm(before, after, cv::NORM_INF) == 0.0;
}
#endif

// std::function needs a copyable callable; a command that carries something move-only holds it here.
template <typename T> std::shared_ptr<T> held(T value) { return std::make_shared<T>(std::move(value)); }

} // namespace

Engine::Engine(Mode mode, EngineHooks hooks)
    : m_mode(mode), m_hooks(std::move(hooks)), m_processor(makeProcessor(ProcessorKind::None)) {
    if (m_mode == Mode::Threaded) {
        m_thread = std::thread([this] { run(); });
    }
}

Engine::~Engine() {
    {
        const std::lock_guard lock(m_mutex);
        m_quit = true;
        m_signalled = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    } else {
        closeDown();
    }
    std::vector<std::unique_ptr<frame_io::RecordingWriter>> finishing;
    std::unique_ptr<ResultSender> sender;
    std::vector<Retired> retiring;
    std::deque<std::function<void()>> commands;
    std::optional<Pending> pending;
    {
        const std::lock_guard lock(m_mutex);
        finishing = std::move(m_finishing);
        m_finishing.clear();
        sender = std::move(m_sender);
        retiring = std::move(m_retiring);
        m_retiring.clear();
        commands = std::move(m_commands);
        m_commands.clear();
        pending = std::move(m_pending);
        m_pending.reset();
    }
    finishing.clear(); // each writes its queue first
    sender.reset();    // waits for a send in progress
    for (Retired& retired : retiring) {
        retired.done.wait();
    }
}

// --- What the window calls
// ----------------------------------------------------------------------------------

void Engine::startLive(std::string name) {
    std::optional<Pending> old; // destroyed without the lock
    std::uint64_t run = 0;
    {
        const std::lock_guard lock(m_mutex);
        old = std::move(m_pending);
        m_pending.reset();
        m_seekRequest.reset(); // for the replay this ends
        m_acceptLive = true;
        m_offered = 0;
        run = ++m_run;
    }
    post([this, name = std::move(name), run] { doStartLive(name, run); });
}

void Engine::offer(PendingFrame frame) {
    std::optional<Pending> replaced; // destroyed without the lock
    {
        const std::lock_guard lock(m_mutex);
        if (!m_acceptLive) {
            return; // no camera run: `frame` goes when this returns
        }
        if (m_pending) {
            replaced = std::move(m_pending);
            ++m_status.droppedBeforeProcessing;
        }
        m_pending = Pending{.frame = std::move(frame), .number = m_offered++, .run = m_run};
        m_signalled = true;
    }
    m_wake.notify_all();
}

void Engine::startReplay(ReplayOpener open) {
    std::optional<Pending> old;
    {
        const std::lock_guard lock(m_mutex);
        old = std::move(m_pending);
        m_pending.reset();
        m_seekRequest.reset(); // for the source this replaces
        m_acceptLive = false;
        ++m_run;
    }
    post([this, open = std::move(open)] { doStartReplay(open); });
}

void Engine::stop() {
    std::optional<Pending> old;
    {
        const std::lock_guard lock(m_mutex);
        old = std::move(m_pending);
        m_pending.reset();
        m_seekRequest.reset();
        m_acceptLive = false;
        ++m_run;
    }
    post([this] { doStop(); });
}

void Engine::play() {
    post([this] { doPlay(); });
}

void Engine::pause() {
    post([this] { doPause(); });
}

void Engine::step() {
    post([this] { doStep(); });
}

void Engine::seek(std::uint64_t index) {
    bool first = false;
    {
        const std::lock_guard lock(m_mutex);
        first = !m_seekRequest;
        m_seekRequest = SeekRequest{.absolute = index, .relative = 0};
    }
    if (first) {
        post([this] { doSeek(); });
    }
}

void Engine::seekBy(std::int64_t frames) {
    bool first = false;
    {
        const std::lock_guard lock(m_mutex);
        first = !m_seekRequest;
        if (first) {
            m_seekRequest = SeekRequest{};
        }
        m_seekRequest->relative += frames;
    }
    if (first) {
        post([this] { doSeek(); });
    }
}

void Engine::stepBack() { seekBy(-1); }

void Engine::restart() {
    {
        const std::lock_guard lock(m_mutex);
        m_seekRequest.reset(); // for the run the restart ends
    }
    post([this] { doRestart(); });
}

void Engine::setLoop(bool on) {
    post([this, on] { doSetLoop(on); });
}

void Engine::setAsFastAsPossible(bool on) {
    post([this, on] { doSetAsFastAsPossible(on); });
}

void Engine::setProcessor(std::unique_ptr<FrameProcessor> processor) {
    post([this, processor = held(std::move(processor))] { doSetProcessor(std::move(*processor)); });
}

void Engine::startRecording(std::unique_ptr<frame_io::RecordingWriter> writer) {
    // The counts start now, when the user asked, not when the processing thread gets to it.
    Totals base;
    {
        const std::lock_guard lock(m_mutex);
        base = totals();
    }
    post([this, writer = held(std::move(writer)), base] { doStartRecording(std::move(*writer), base); });
}

void Engine::stopRecording() {
    post([this] { stopRecordingNow(); });
}

void Engine::setSender(std::unique_ptr<ResultSender> sender) {
    post([this, sender = held(std::move(sender))] { doSetSender(std::move(*sender)); });
}

void Engine::pauseSending(bool paused) {
    post([this, paused] { doPauseSending(paused); });
}

void Engine::stopSending(bool endChannels) {
    if (endChannels) {
        // From the call, not from when the processing thread gets to it: the window disables ▶ at once.
        const std::lock_guard lock(m_mutex);
        ++m_endsAsked;
        m_status.endingChannels = true;
    }
    post([this, endChannels] { doStopSending(endChannels); });
}

void Engine::reportNotDrawn(std::uint64_t total) {
    const std::lock_guard lock(m_mutex);
    m_notDrawn = total;
}

std::optional<LabResult> Engine::takeResult() {
    std::optional<LabResult> result;
    const std::lock_guard lock(m_mutex);
    result.swap(m_slot);
    return result;
}

EngineStatus Engine::status() const {
    const std::lock_guard lock(m_mutex);
    EngineStatus status = m_status;
    if (m_recorder) {
        status.recordingProgress = m_recorder->progress();
    } else if (!m_finishing.empty()) {
        status.recordingProgress = m_finishing.back()->progress();
    } else {
        status.recordingProgress = m_lastRecording;
    }
    if (m_sender) {
        status.sending = m_sender->stats();
    }
    return status;
}

bool Engine::shutDown() const {
    const std::lock_guard lock(m_mutex);
    if (!m_commands.empty() || m_status.busyFrame || isRunning(m_status.state) || m_recorder) {
        return false;
    }
    for (const auto& writer : m_finishing) {
        if (!writer->progress().finished) {
            return false;
        }
    }
    for (const Retired& retired : m_retiring) {
        if (retired.done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false;
        }
    }
    return m_endsAsked == 0;
}

bool Engine::pump() {
    if (m_mode != Mode::Immediate) {
        return false;
    }
    return turn().did;
}

// --- The processing loop
// ------------------------------------------------------------------------------------

void Engine::post(std::function<void()> command) {
    {
        const std::lock_guard lock(m_mutex);
        m_commands.push_back(std::move(command));
        m_signalled = true;
    }
    m_wake.notify_all();
}

void Engine::signal() {
    {
        const std::lock_guard lock(m_mutex);
        m_signalled = true;
    }
    m_wake.notify_all();
}

void Engine::run() {
    for (;;) {
        const Turn done = turn();
        std::unique_lock lock(m_mutex);
        if (m_quit) {
            break;
        }
        if (done.did) {
            continue;
        }
        if (done.waitForRecorder) {
            lock.unlock();
            // The recorder's own wait, in slices, so a command never waits long.
            m_recorder->waitForSpace(kWaitSlice, bytesOf(*m_waitingToRecord));
            continue;
        }
        std::chrono::nanoseconds timeout =
            m_finishing.empty() && m_retiring.empty() ? std::chrono::nanoseconds(kIdleWait) : kWaitSlice;
        if (done.wakeAt) {
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(*done.wakeAt - now());
            timeout =
                std::clamp(remaining, std::chrono::nanoseconds(0), std::chrono::nanoseconds(kWaitSlice));
        }
        m_wake.wait_for(lock, timeout, [this] { return m_signalled; });
        m_signalled = false;
    }
    closeDown();
}

// The commands waiting, then at most one frame; the hooks after, with no lock held. Nothing thrown leaves:
// on the processing thread it would end the program.
Engine::Turn Engine::turn() {
    Turn turn;
    try {
        for (;;) {
            std::function<void()> command;
            {
                const std::lock_guard lock(m_mutex);
                if (m_commands.empty()) {
                    break;
                }
                command = std::move(m_commands.front());
                m_commands.pop_front();
            }
            command();
            turn.did = true;
        }
        pruneFinished();
        stopFailedRecording();
        if (m_live) {
            liveWork(turn);
        } else if (m_source) {
            replayWork(turn);
        }
    } catch (const std::exception& exception) {
        turn = {.did = true, .wakeAt = {}, .waitForRecorder = false};
        stopAfterThrow(exception.what());
    } catch (...) {
        turn = {.did = true, .wakeAt = {}, .waitForRecorder = false};
        stopAfterThrow("unknown exception");
    }
    notify();
    return turn;
}

// A recording whose writer failed in the background stops now, not at the next frame it is given.
void Engine::stopFailedRecording() {
    if (!m_recorder) {
        return;
    }
    const std::string failure = m_recorder->progress().failure;
    if (!failure.empty()) {
        stopRecordingNow(kRecordingStopped + failure);
    }
}

// Everything that was going on stops, as cleanly as it can: whatever threw may have left a frame half-done.
void Engine::stopAfterThrow(const std::string& what) {
    std::optional<Pending> pending; // destroyed without the lock
    {
        const std::lock_guard lock(m_mutex);
        m_status.busyFrame.reset();
        m_status.busySince.reset();
        m_seekRequest.reset();
        pending = std::move(m_pending);
        m_pending.reset();
        m_acceptLive = false;
    }
    m_held.reset();
    m_waitingToRecord.reset();
    try {
        stopRecordingNow();
    } catch (...) {
        // It could not be finished in the background: its destructor writes its queue here instead.
        std::unique_ptr<frame_io::RecordingWriter> dropped;
        {
            const std::lock_guard lock(m_mutex);
            dropped = std::move(m_recorder);
            m_status.recording = false;
        }
    }
    closeSource();
    m_live = false;
    setMessage("stopped: internal error (" + what + ")");
    setState(EngineState::Stopped);
}

void Engine::liveWork(Turn& turn) {
    if (m_state != EngineState::Live) {
        return;
    }
    std::optional<Pending> pending;
    {
        const std::lock_guard lock(m_mutex);
        // A frame of a camera run whose start is still in the mailbox waits for it.
        if (!m_pending || m_pending->run != m_liveRun) {
            return;
        }
        pending = std::move(m_pending);
        m_pending.reset();
    }
    turn.did = true;
    std::string error;
    std::optional<frame_io::Frame> frame;
    try {
        if (pending->frame.convert) {
            frame = pending->frame.convert(error);
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    const std::uint64_t number = pending->number;
    pending.reset(); // the camera's buffer goes back before the processing
    if (!frame) {
        setMessage(error.empty() ? "a camera frame could not be converted" : error);
        return;
    }
    frame->number = number;
    process(std::move(*frame), true);
}

void Engine::replayWork(Turn& turn) {
    // The frame before waits for room in the recorder: the replay waits with it.
    if (m_waitingToRecord) {
        if (!pushWaitingFrame()) {
            turn.waitForRecorder = true;
            return;
        }
        turn.did = true;
    }
    if (m_stepWanted && m_state == EngineState::Ended && m_oneFrame && m_onScreen) {
        m_stepWanted = false;
        process(*m_onScreen, false);
        turn.did = true;
        return;
    }
    const bool stepping = m_stepWanted && m_state == EngineState::Paused;
    if (m_state != EngineState::Playing && !stepping) {
        return;
    }
    if (!m_held) {
        if (!m_source->ready()) {
            return; // its wake signals when it is
        }
        frame_io::ReadResult read = m_source->next();
        switch (read.status) {
        case frame_io::ReadStatus::End:
            turn.did = true;
            m_stepWanted = false;
            clearSeeking(); // a seek past the last frame, when the count said one more
            if (m_loop && !m_oneFrame && m_state == EngineState::Playing) {
                newGeneration();
                std::string error;
                if (!m_source->restart(error)) {
                    stopWithError("could not start again: " + error);
                    return;
                }
                m_nextIndex = 0;
            } else {
                setState(EngineState::Ended);
            }
            return;
        case frame_io::ReadStatus::Error:
            turn.did = true;
            m_stepWanted = false;
            clearSeeking();
            stopWithError("stopped at frame " + std::to_string(m_nextIndex + 1) + ": " + read.error);
            return;
        case frame_io::ReadStatus::Frame:
            m_nextIndex = read.frame.number + 1;
            m_held = std::move(read.frame);
            break;
        }
    }
    if (!stepping && !m_asFast) {
        if (!m_heldDue) {
            m_heldDue = m_pacer.due(m_held->at, now());
        }
        if (now() < *m_heldDue) {
            turn.wakeAt = *m_heldDue;
            return;
        }
    }
    frame_io::Frame frame = std::move(*m_held);
    m_held.reset();
    m_heldDue.reset();
    m_stepWanted = false;
    process(std::move(frame), true);
    clearSeeking(); // the seek's frame is on screen
    turn.did = true;
}

// The frame reaches the recorder, the sender and the slot only after process() returns.
void Engine::process(frame_io::Frame frame, bool record) {
    const Clock::time_point start = now();
    {
        const std::lock_guard lock(m_mutex);
        m_status.busyFrame = frame.number;
        m_status.busySince = start;
    }
#if !defined(NDEBUG)
    const cv::Mat before = frame.image.clone();
#endif
    LabResult result;
    result.generation = m_generation;
    try {
        result.processed = m_processor->process(frame);
    } catch (const std::exception& exception) {
        result.processorError = *exception.what() ? exception.what() : "the processor failed";
    } catch (...) {
        result.processorError = "the processor failed";
    }
    result.processingTime = now() - start;
#if !defined(NDEBUG)
    if (result.processorError.empty() && !samePixels(before, frame.image)) {
        result.processorError = "the processor changed its input frame";
    }
#endif
    if (record) {
        this->record(frame);
    }
    const bool canStepBack = !m_live && m_source && m_source->canSeek() && frame.number > 0;
    m_onScreen = frame;
    result.frame = std::move(frame);
    if (m_sender) {
        m_sender->submit(result);
    }
    m_lastResult = result;             // for a sender set or resumed while nothing runs
    std::optional<LabResult> replaced; // destroyed without the lock
    const std::lock_guard lock(m_mutex);
    m_status.busyFrame.reset();
    m_status.busySince.reset();
    m_status.frameNumber = result.frame.number;
    m_status.canStepBack = canStepBack;
    if (!result.processorError.empty()) {
        ++m_status.processorErrors;
    }
    if (m_slot) {
        ++m_status.resultsNotTaken;
        replaced = std::move(m_slot);
    } else {
        m_notifyResult = true;
    }
    m_slot = std::move(result);
}

// A camera's frame that finds the queue full is left out and counted; a replay's waits for room.
void Engine::record(const frame_io::Frame& frame) {
    if (!m_recorder) {
        return;
    }
    switch (m_recorder->push(frame)) {
    case frame_io::RecordingWriter::Push::Queued:
        return;
    case frame_io::RecordingWriter::Push::Full:
        if (m_live) {
            m_recorder->countNotRecorded();
        } else {
            m_waitingToRecord = frame;
        }
        return;
    case frame_io::RecordingWriter::Push::LowDisk:
        m_recorder->countNotRecorded();
        stopRecordingNow(kLowDisk);
        return;
    case frame_io::RecordingWriter::Push::Failed:
        m_recorder->countNotRecorded();
        stopRecordingNow(kRecordingStopped + m_recorder->progress().failure);
        return;
    }
}

// Whether the frame waiting for room has gone, into the queue or out of a recording that stopped.
bool Engine::pushWaitingFrame() {
    if (!m_recorder) {
        m_waitingToRecord.reset();
        return true;
    }
    switch (m_recorder->push(*m_waitingToRecord)) {
    case frame_io::RecordingWriter::Push::Queued:
        m_waitingToRecord.reset();
        return true;
    case frame_io::RecordingWriter::Push::Full:
        return false;
    case frame_io::RecordingWriter::Push::LowDisk:
        m_waitingToRecord.reset();
        m_recorder->countNotRecorded();
        stopRecordingNow(kLowDisk);
        return true;
    case frame_io::RecordingWriter::Push::Failed:
        m_waitingToRecord.reset();
        m_recorder->countNotRecorded();
        stopRecordingNow(kRecordingStopped + m_recorder->progress().failure);
        return true;
    }
    return true;
}

void Engine::notify() {
    const bool status = std::exchange(m_notifyStatus, false);
    const bool result = std::exchange(m_notifyResult, false);
    if (status && m_hooks.statusChanged) {
        m_hooks.statusChanged();
    }
    if (result && m_hooks.resultReady) {
        m_hooks.resultReady();
    }
}

// Recordings that have written everything go, and retired senders that are gone; destroyed without the lock.
void Engine::pruneFinished() {
    std::vector<std::unique_ptr<frame_io::RecordingWriter>> finished;
    std::vector<std::future<void>> retired;
    const std::lock_guard lock(m_mutex);
    for (auto writer = m_finishing.begin(); writer != m_finishing.end();) {
        const frame_io::RecordingWriter::Progress progress = (*writer)->progress();
        if (progress.finished) {
            m_lastRecording = progress;
            finished.push_back(std::move(*writer));
            writer = m_finishing.erase(writer);
            m_notifyStatus = true;
        } else {
            ++writer;
        }
    }
    bool ending = m_endsAsked > 0;
    for (auto sender = m_retiring.begin(); sender != m_retiring.end();) {
        if (sender->done.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            retired.push_back(std::move(sender->done));
            sender = m_retiring.erase(sender);
        } else {
            ending = ending || sender->endingChannels;
            ++sender;
        }
    }
    if (ending != m_status.endingChannels) {
        m_status.endingChannels = ending;
        m_notifyStatus = true;
    }
}

void Engine::closeDown() {
    stopRecordingNow();
    closeSource();
    m_live = false;
}

// --- Commands, on the processing thread
// ---------------------------------------------------------------------

void Engine::doStartLive(const std::string& name, std::uint64_t run) {
    stopRecordingNow();
    closeSource();
    m_live = true;
    m_liveRun = run;
    newGeneration();
    m_onScreen.reset();
    {
        const std::lock_guard lock(m_mutex);
        m_status.source = name;
        m_status.frameNumber.reset();
        m_status.frameCount.reset();
        m_status.canStepBack = false;
        m_status.loop = m_loop;
    }
    setMessage({});
    setState(EngineState::Live);
}

void Engine::doStartReplay(const ReplayOpener& open) {
    stopRecordingNow();
    closeSource();
    m_live = false;
    newGeneration();
    m_onScreen.reset();
    std::string error;
    std::unique_ptr<frame_io::ReplaySource> source;
    try {
        if (open) {
            source = open(error);
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    if (!source) {
        setMessage(error.empty() ? "the source could not be opened" : error);
        setState(EngineState::Stopped);
        return;
    }
    source->setWake([this] { signal(); });
    const std::optional<std::uint64_t> count = source->frameCount();
    const std::optional<double> rate = source->frameRate();
    const bool canSeek = source->canSeek();
    const std::string name = source->describe();
    m_source = std::move(source);
    m_oneFrame = count == std::uint64_t{1};
    m_nextIndex = 0;
    {
        const std::lock_guard lock(m_mutex);
        m_status.source = name;
        m_status.frameNumber.reset();
        m_status.frameCount = count;
        m_status.frameRate = rate;
        m_status.canSeek = canSeek;
        m_status.canStepBack = false;
        m_status.loop = m_loop && !m_oneFrame;
    }
    setMessage({});
    setState(EngineState::Playing);
}

void Engine::doStop() {
    stopRecordingNow();
    closeSource();
    m_live = false;
    setMessage({});
    setState(EngineState::Stopped);
}

void Engine::doPlay() {
    if (m_state == EngineState::Paused) {
        m_pacer.restart();
        m_heldDue.reset();
        setState(EngineState::Playing);
    } else if (m_state == EngineState::Ended && !m_oneFrame) {
        doRestart(); // played to its end: again from the start, as players do
    }
}

void Engine::doPause() {
    if (m_state == EngineState::Playing) {
        m_heldDue.reset();
        setState(EngineState::Paused);
    }
}

void Engine::doStep() {
    if (m_live || !m_source) {
        return;
    }
    if (m_state == EngineState::Playing) {
        m_heldDue.reset();
        setState(EngineState::Paused);
    }
    if (m_state == EngineState::Paused || (m_state == EngineState::Ended && m_oneFrame)) {
        m_stepWanted = true;
    }
}

// Steps count from the seek still running, else from the frame on screen; in signed arithmetic, so a step
// back from frame 0 stays there rather than wrapping round to the last frame.
void Engine::doSeek() {
    std::optional<SeekRequest> request;
    {
        const std::lock_guard lock(m_mutex);
        request.swap(m_seekRequest);
    }
    if (!request || m_live || !m_source || !m_source->canSeek()) {
        return;
    }
    const std::uint64_t from = request->absolute ? *request->absolute
                               : m_seekTarget    ? *m_seekTarget
                               : m_onScreen      ? m_onScreen->number
                                                 : m_nextIndex;
    std::int64_t wanted = std::max<std::int64_t>(0, static_cast<std::int64_t>(from) + request->relative);
    const std::optional<std::uint64_t> count = m_source->frameCount();
    if (count && *count > 0) {
        wanted = std::min(wanted, static_cast<std::int64_t>(*count - 1));
    }
    const auto target = static_cast<std::uint64_t>(wanted);
    // A step back at the first frame, paused: nothing to do, and no reason to end a recording.
    if (m_state == EngineState::Paused && !m_seekTarget && m_onScreen && m_onScreen->number == target) {
        return;
    }
    const bool playing = m_state == EngineState::Playing;
    setMessage({});
    stopRecordingNow(kReplayMoved);
    newGeneration();
    m_source->seek(target);
    m_nextIndex = target;
    m_seekTarget = target;
    {
        const std::lock_guard lock(m_mutex);
        m_status.seeking = true;
        m_status.seekTarget = target;
    }
    m_notifyStatus = true;
    if (playing) {
        return; // plays on from the target; newGeneration() restarted the pacer
    }
    setState(EngineState::Paused);
    m_stepWanted = true;
}

void Engine::doRestart() {
    if (m_live || !m_source) {
        return;
    }
    const bool paused = m_state == EngineState::Paused;
    newGeneration();
    std::string error;
    if (!m_source->restart(error)) {
        stopWithError("could not start again: " + error);
        return;
    }
    m_nextIndex = 0;
    setMessage({});
    if (paused) {
        m_stepWanted = true; // shows frame 0, still paused
    } else {
        setState(EngineState::Playing);
    }
}

void Engine::doSetLoop(bool on) {
    m_loop = on;
    const std::lock_guard lock(m_mutex);
    m_status.loop = on && !m_oneFrame;
}

void Engine::doSetAsFastAsPossible(bool on) {
    m_asFast = on;
    if (!on) {
        m_pacer.restart();
        m_heldDue.reset();
    }
    const std::lock_guard lock(m_mutex);
    m_status.asFastAsPossible = on;
}

void Engine::doSetProcessor(std::unique_ptr<FrameProcessor> processor) {
    m_processor = processor ? std::move(processor) : makeProcessor(ProcessorKind::None);
    resetProcessor();
    // Frames that keep coming show the new results soon enough; a paused replay or an image needs this.
    if (m_onScreen && !isRunning(m_state)) {
        process(*m_onScreen, false);
    }
}

void Engine::doStartRecording(std::unique_ptr<frame_io::RecordingWriter> writer, Totals base) {
    stopRecordingNow();
    if (!writer) {
        return;
    }
    m_recordingBase = base;
    {
        const std::lock_guard lock(m_mutex);
        m_recorder = std::move(writer);
        m_status.recording = true;
    }
    m_notifyStatus = true;
}

// While nothing runs (paused, ended, one picture) the new sender gets the frame on screen at once.
void Engine::doSetSender(std::unique_ptr<ResultSender> sender) {
    retireSender(false);
    {
        const std::lock_guard lock(m_mutex);
        m_sender = std::move(sender);
    }
    if (m_sender && !isRunning(m_state) && m_lastResult) {
        m_sender->submit(*m_lastResult);
    }
}

void Engine::doPauseSending(bool paused) {
    if (!m_sender) {
        return;
    }
    m_sender->setPaused(paused);
    if (!paused && !isRunning(m_state) && m_lastResult) {
        m_sender->submit(*m_lastResult);
    }
}

void Engine::doStopSending(bool endChannels) {
    retireSender(endChannels);
    if (endChannels) {
        const std::lock_guard lock(m_mutex);
        --m_endsAsked; // the retiree, if there was one, now says whether it is ending
    }
}

// The sender goes on a thread of its own, after ending its channels when asked, so a send in progress to a
// slow viewer never holds up the processing or the window.
void Engine::retireSender(bool endChannels) {
    std::unique_ptr<ResultSender> old;
    {
        const std::lock_guard lock(m_mutex);
        old = std::move(m_sender);
        if (old) {
            m_notSentRetired += old->stats().notSent;
        }
    }
    if (!old) {
        return;
    }
    try {
        std::future<void> retiring =
            std::async(std::launch::async, [retired = std::move(old), endChannels]() mutable {
                if (endChannels) {
                    retired->endChannels();
                }
                retired.reset();
            });
        const std::lock_guard lock(m_mutex);
        m_retiring.push_back({.done = std::move(retiring), .endingChannels = endChannels});
    } catch (const std::system_error&) {
        // No thread to spare: the sender went with the task that could not start, here, after its send, and
        // without ending its channels.
    }
}

// --- Helpers, on the processing thread
// ----------------------------------------------------------------------

void Engine::newGeneration() {
    stopRecordingNow();
    ++m_generation;
    std::optional<LabResult> stale; // destroyed without the lock
    {
        const std::lock_guard lock(m_mutex);
        m_status.generation = m_generation;
        stale.swap(m_slot);
    }
    resetProcessor();
    m_held.reset();
    m_heldDue.reset();
    m_stepWanted = false;
    m_pacer.restart();
    clearSeeking();
}

// The seek's frame was shown, or will never be: the end, an error, a new run.
void Engine::clearSeeking() {
    if (!m_seekTarget) {
        return;
    }
    m_seekTarget.reset();
    {
        const std::lock_guard lock(m_mutex);
        m_status.seeking = false;
        m_status.seekTarget.reset();
    }
    m_notifyStatus = true;
}

// Its counts are those since the recording started; the writer writes its queue in the background.
void Engine::stopRecordingNow(const std::string& message) {
    if (!m_recorder) {
        return;
    }
    if (m_waitingToRecord) {
        // One more try: the queue may have room by now.
        if (m_recorder->push(*m_waitingToRecord) != frame_io::RecordingWriter::Push::Queued) {
            m_recorder->countNotRecorded();
        }
        m_waitingToRecord.reset();
    }
    Totals current;
    {
        const std::lock_guard lock(m_mutex);
        current = totals();
    }
    m_recorder->setCounts({.droppedBeforeProcessing = since(current.dropped, m_recordingBase.dropped),
                           .notDrawn = since(current.notDrawn, m_recordingBase.notDrawn) +
                                       since(current.notTaken, m_recordingBase.notTaken),
                           .notSent = since(current.notSent, m_recordingBase.notSent),
                           .notRecorded = m_recorder->progress().notRecorded});
    m_recorder->finishAsync();
    {
        const std::lock_guard lock(m_mutex);
        m_finishing.push_back(std::move(m_recorder));
        m_recorder.reset();
        m_status.recording = false;
        if (!message.empty()) {
            m_status.message = message;
        }
    }
    m_notifyStatus = true;
}

void Engine::stopWithError(const std::string& message) {
    stopRecordingNow();
    setMessage(message);
    setState(EngineState::Stopped);
}

void Engine::setState(EngineState state) {
    if (state == m_state) {
        return;
    }
    m_state = state;
    {
        const std::lock_guard lock(m_mutex);
        m_status.state = state;
    }
    m_notifyStatus = true;
}

void Engine::setMessage(std::string message) {
    const std::lock_guard lock(m_mutex);
    if (m_status.message == message) {
        return;
    }
    m_status.message = std::move(message);
    m_notifyStatus = true;
}

void Engine::resetProcessor() {
    try {
        m_processor->reset();
    } catch (...) {
        const std::lock_guard lock(m_mutex);
        ++m_status.processorErrors;
    }
}

// Without the lock: a source joins its reading threads, which may be calling the wake.
void Engine::closeSource() {
    m_source.reset();
    m_held.reset();
    m_heldDue.reset();
    m_oneFrame = false;
    m_stepWanted = false;
    m_nextIndex = 0;
    m_lastResult.reset(); // the source's: not for a sender set after it
    clearSeeking();
    const std::lock_guard lock(m_mutex);
    m_status.frameRate.reset();
    m_status.canSeek = false;
}

Engine::Totals Engine::totals() const {
    return {.dropped = m_status.droppedBeforeProcessing,
            .notTaken = m_status.resultsNotTaken,
            .notDrawn = m_notDrawn,
            .notSent = m_notSentRetired + (m_sender ? m_sender->stats().notSent : 0)};
}

Clock::time_point Engine::now() const { return m_hooks.now ? m_hooks.now() : Clock::now(); }

} // namespace lab_core
