#include "snapshot_converter.h"

#include <QMetaObject>

#include <utility>

cv::Mat convertSnapshot(const SnapshotJob& job) {
    return viewer_core::historyDisplay(job.original, job.origin.tags);
}

SnapshotConverter::SnapshotConverter(Mode mode, Receiver receiver, Conversion conversion, QObject* parent)
    : QObject(parent), m_mode(mode), m_receiver(std::move(receiver)), m_conversion(std::move(conversion)) {
    if (m_mode == Mode::Threaded) {
        m_thread = std::thread([this] { run(); });
    }
}

SnapshotConverter::~SnapshotConverter() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_waiting.clear();
    }
    m_wake.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // A result posted before the thread stopped is removed with this object's posted events by ~QObject.
}

void SnapshotConverter::submit(SnapshotJob job) {
    if (m_mode == Mode::Immediate) {
        m_receiver(convert(std::move(job)));
        return;
    }
    {
        const std::lock_guard lock(m_mutex);
        m_waiting.push_back(std::move(job));
    }
    m_wake.notify_one();
}

void SnapshotConverter::run() {
    for (;;) {
        SnapshotJob job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_waiting.empty(); });
            if (m_stopping) {
                return;
            }
            job = std::move(m_waiting.front());
            m_waiting.pop_front();
        }
        SnapshotResult result = convert(std::move(job));
        const std::lock_guard lock(m_mutex);
        if (m_stopping) {
            return;
        }
        // Queued to this object, which lives on the UI thread: the receiver runs there, in the order taken.
        QMetaObject::invokeMethod(
            this, [this, converted = std::move(result)]() mutable { m_receiver(std::move(converted)); },
            Qt::QueuedConnection);
    }
}

SnapshotResult SnapshotConverter::convert(SnapshotJob job) const {
    SnapshotResult result{.job = std::move(job)};
    try {
        result.display = m_conversion(result.job);
    } catch (...) { // out of memory, say: the window says the snapshot could not be made
        result.display = cv::Mat();
    }
    return result;
}
