#pragma once

#include "frame_io/frame.h"
#include "frame_io/replay_source.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frame_io {

// Makes a new folder `baseName` in `parent`, or "baseName 2", "baseName 3"… when the name is taken, never one
// that already exists. Its path, or empty with `error`.
std::filesystem::path createUniqueFolder(const std::filesystem::path& parent, const std::string& baseName,
                                         std::string& error);

// Where a recording's frames came from, for its recording.json.
struct RecordingInfo {
    std::string kind;      // "camera", "video", "images" or "recording"
    std::string name;      // the camera's or the file's
    std::string format;    // a camera's format, such as "1920×1440, 60 fps, NV12"; empty for a file
    std::string startedAt; // local time, ISO 8601
};

// The frames that did not reach every output while recording, for its recording.json.
struct RecordingCounts {
    std::uint64_t droppedBeforeProcessing = 0;
    std::uint64_t notDrawn = 0;
    std::uint64_t notSent = 0;
    std::uint64_t notRecorded = 0;
};

// Writes a recording: each frame pushed as a lossless PNG (000000.png, 000001.png, …, 8- or 16-bit as the
// frame is) on writer threads, and recording.json, written when the recording starts marked incomplete and
// again, complete, when it finishes. Files are made readable by their owner only on macOS and Linux; on
// Windows they inherit the folder's access list, which the caller sets.
class RecordingWriter {
public:
    enum class Mode { Threaded, Immediate };
    struct Options {
        Mode mode = Mode::Threaded;
        // Six beat four at a camera's 1920×1440 by half again on the Mac mini M4 Pro (the measurement in
        // recording_measurement_test.cpp), headroom for 60 fps.
        int threads = 6;
        // The frames queued and being written may take this many bytes; a push past it finds the queue full.
        std::size_t queueBytes = std::size_t{1} << 30;
        std::uintmax_t minimumFreeBytes = 5'000'000'000; // below it, a push reports LowDisk
        std::function<std::uintmax_t(const std::filesystem::path&)> freeSpace; // null: what the volume has
        std::function<void(std::uint64_t index)> beforeWrite;                  // tests hold a writer here
    };
    // Failed: a frame could not be written (progress().failure says which); the recording takes no more.
    enum class Push { Queued, Full, LowDisk, Failed };
    struct Progress {
        std::uint64_t written = 0;
        std::uint64_t queued = 0; // pushed and not yet written
        std::uint64_t notRecorded = 0;
        bool finished = false;
        std::string failure; // the first frame or file that could not be written; empty when none
    };

    // Starts a recording in `folder`, which must exist (the caller has made it private), by writing its
    // recording.json. Null, with `error`, when that cannot be written.
    static std::unique_ptr<RecordingWriter> start(std::filesystem::path folder, RecordingInfo info,
                                                  Options options, std::string& error);

    // Queues the frame without waiting or copying its pixels. Its time is kept from the first frame pushed.
    // Full: the queue has no room; the frame is not recorded. LowDisk: the volume has too little room left.
    Push push(const Frame& frame);
    // Waits until the queue has room for a frame of `bytes` (any room at all when 0), so that a push of it
    // would be queued, or until `timeout` passes; whether it has.
    bool waitForSpace(std::chrono::milliseconds timeout, std::size_t bytes = 0);
    // Immediate mode: writes every queued frame now. Whether there were any.
    bool pump();
    void countNotRecorded(); // a frame left out because the queue was full
    void setCounts(const RecordingCounts& counts);
    Progress progress() const;
    // Lets the writers finish the queue, then writes recording.json complete; returns at once (in Immediate
    // mode it has done it all when it returns). progress().finished says when it is done.
    void finishAsync();
    void finish(); // finishAsync(), then waits for it
    ~RecordingWriter();

    RecordingWriter(const RecordingWriter&) = delete;
    RecordingWriter& operator=(const RecordingWriter&) = delete;

private:
    struct Entry {
        std::string file;
        std::chrono::nanoseconds at{};
        visual_debug::ImageDescription tags;
        bool written = false; // only written frames go in recording.json
    };
    struct Job {
        std::uint64_t index = 0;
        cv::Mat image;
        std::filesystem::path file;
        std::size_t bytes = 0;
    };

    RecordingWriter(std::filesystem::path folder, RecordingInfo info, Options options);
    void writeLoop();
    void write(Job job);                                     // without the lock
    void wrote(const Job& job, const std::string& failure);  // with it
    bool dropAfterFailure(const Job& job);                   // with it: whether the job is not to be written
    bool writeJson(bool complete, std::string& error) const; // with it
    void completeIfDone();                                   // with it

    std::filesystem::path m_folder;
    RecordingInfo m_info;
    Options m_options;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Job> m_jobs;
    std::size_t m_bytes = 0; // queued and being written
    int m_writing = 0;
    std::uint64_t m_written = 0;
    std::uint64_t m_notRecorded = 0;
    bool m_finishing = false;
    bool m_finished = false;
    std::string m_failure;
    std::vector<Entry> m_entries;
    std::optional<std::chrono::nanoseconds> m_firstAt;
    RecordingCounts m_counts;
    std::vector<std::thread> m_threads; // last, so they start after everything they use
};

// A recording as a replay source: its frames in recorded order, at the recorded times, with the recorded
// tags. One whose recording.json says it is incomplete (cut short by a crash) replays the frames it lists,
// then any PNGs it does not, in name order, a thirtieth of a second apart. Null, with `error`, without a
// readable recording.json.
std::unique_ptr<ReplaySource> openRecording(const std::filesystem::path& folder, std::string& error);

// Whatever `path` is: a folder with a recording.json is a recording, any other folder a folder of images; a
// file with an image's extension is an image, any other file a video.
std::unique_ptr<ReplaySource> openReplay(const std::filesystem::path& path, std::string& error);

} // namespace frame_io
