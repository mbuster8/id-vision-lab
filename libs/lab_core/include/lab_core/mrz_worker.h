#pragma once

#include "lab_core/frame_processor.h"

#include <doc_vision/mrz_reader.h>
#include <doc_vision/onnx_model.h>
#include <doc_vision/page_geometry.h>
#include <frame_io/frame.h>

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lab_core {

// What the Document processor remembers of a frame it hands to the reader. It travels with the job and comes
// back with its read, so a job replaced while it waits frees its frame with it.
struct MrzFrameRecord {
    frame_io::Frame frame;                  // shared and read-only: the captured frame, if this read wins
    doc_vision::Quad quad{};                // the page's, in the frame
    double sharpness = 0.0;                 // PageFacts::sharpness
    std::vector<Finding> gates;             // the frame's Fill, Tilt, Sharpness, Glare and Exposure rows
    std::chrono::nanoseconds firstPageAt{}; // the read window's first fresh page, for the time to scan
};

struct MrzJob {
    cv::Mat bgr; // the frame, 8-bit BGR, shared and read-only
    MrzFrameRecord record;
    bool turnFirst = false; // read the page turned 180° first
    std::uint64_t resetCount = 0;
};

struct MrzDone {
    MrzFrameRecord record;
    std::uint64_t resetCount = 0;
    doc_vision::MrzReading reading; // its band on the page as read: turned when `turned`
    bool turned = false;
    std::string error; // fixed words or ONNX Runtime's own message, never read characters
};

// The Document processor's MRZ reader on a thread of its own (spec §4.2), so guidance keeps its frame rate: a
// slot of one for the job waiting (a newer one replaces it), one read at a time. The reader is made on the
// worker's thread on the first job and kept. Its end never waits a whole read: the destructor cancels the
// read in progress, and a reader still opening is never used. Nothing thrown leaves it.
class MrzWorker {
public:
    // Threaded: a thread of its own. Manual: pump() runs the job waiting, for the tests. Immediate: submit()
    // reads at once.
    enum class Mode { Threaded, Manual, Immediate };
    // Null, with `error` and `failure`, when the reader cannot be made.
    using MakeReader = std::function<std::unique_ptr<doc_vision::MrzReader>(
        std::string& error, doc_vision::OpenFailure& failure)>;

    MrzWorker(Mode mode, MakeReader makeReader);
    ~MrzWorker();
    MrzWorker(const MrzWorker&) = delete;
    MrzWorker& operator=(const MrzWorker&) = delete;

    // Replaces a job still waiting; nothing once the reader is unavailable.
    void submit(MrzJob job);
    // The reads finished since the last take, oldest first.
    std::vector<MrzDone> take();
    // Manual: runs the job waiting. Whether there was one.
    bool pump();
    // Set once the reader could not be made: why, in fixed words ("model missing", "ONNX Runtime refused
    // it").
    std::optional<std::string> unavailable() const;
    // A job waiting or being read.
    bool busy() const;

private:
    void run();
    void work(MrzJob job);

    const Mode m_mode;
    MakeReader m_makeReader;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<MrzJob> m_waiting;
    std::vector<MrzDone> m_done;
    bool m_working = false;
    bool m_quit = false;
    bool m_tried = false; // the reader was made, or failed to be
    std::shared_ptr<doc_vision::MrzReader> m_reader;
    std::optional<std::string> m_unavailable;

    std::thread m_thread; // last, so it starts after everything it uses
};

} // namespace lab_core
