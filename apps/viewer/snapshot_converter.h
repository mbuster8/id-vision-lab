#pragma once

#include "viewer_core/image_history.h"

#include <QObject>

#include <opencv2/core/mat.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// A snapshot on its way into the history: what it keeps, and what the window logs once it is there.
struct SnapshotJob {
    std::string name;
    cv::Mat original; // what the history keeps: a cut of the frame, or the whole frame shared
    viewer_core::ImageOrigin origin{};
    std::optional<cv::Rect> area{}; // where it was cut from the frame; nothing for the whole frame
};

struct SnapshotResult {
    SnapshotJob job;
    cv::Mat display{}; // the history's picture of it (historyDisplay); empty if it could not be made
};

// The history's picture of the job's original, as ImageHistory::add would make it.
cv::Mat convertSnapshot(const SnapshotJob& job);

// Makes snapshots' pictures for the history off the UI thread: a 4K HDR frame's takes over 100 ms in a debug
// build, more while the live converter is busy, which would freeze the stream in view. One thread, a queue:
// unlike live frames, no snapshot is ever replaced, and they arrive in the order taken, on the thread the
// converter lives in. Immediate mode converts before submit returns, for tests that must not wait.
class SnapshotConverter : public QObject {
    Q_OBJECT

public:
    enum class Mode { Threaded, Immediate };
    using Conversion = std::function<cv::Mat(const SnapshotJob&)>;
    using Receiver = std::function<void(SnapshotResult)>;

    SnapshotConverter(Mode mode, Receiver receiver, Conversion conversion = convertSnapshot,
                      QObject* parent = nullptr);
    // Waits for a conversion in progress; snapshots still waiting are dropped, and nothing reaches the
    // receiver.
    ~SnapshotConverter() override;

    SnapshotConverter(const SnapshotConverter&) = delete;
    SnapshotConverter& operator=(const SnapshotConverter&) = delete;

    void submit(SnapshotJob job);

private:
    void run();
    SnapshotResult convert(SnapshotJob job) const; // a conversion that throws gives no picture

    Mode m_mode;
    Receiver m_receiver;
    Conversion m_conversion;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<SnapshotJob> m_waiting; // guarded by m_mutex
    bool m_stopping = false;           // guarded by m_mutex
    std::thread m_thread;              // last, so it starts once the rest exists
};
