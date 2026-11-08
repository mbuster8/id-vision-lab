#include "live_converter.h"

#include "qimage_conversion.h"

#include "viewer_core/display_converter.h"

#include <QMetaObject>

#include <utility>

LiveResult convertLiveFrame(const LiveJob& job) {
    LiveResult result{.job = job, .imageSize = QSize(job.frame.cols, job.frame.rows)};
    result.picture = qt_common::wrapAsQImage(viewer_core::toDisplayImage(
        job.frame, job.settings,
        {.size = job.drawnSize, .tags = job.tags, .showClipping = job.showClipping}));
    if (job.histogram) {
        const viewer_core::ValueExtent extent = viewer_core::valueExtent(job.frame);
        result.markers = viewer_core::rangeMarkers(job.frame.depth(), job.frame.channels(), job.settings,
                                                   job.tags, extent);
        result.histogram = viewer_core::histogramOf(job.frame, extent, result.markers);
    }
    return result;
}

LiveConverter::LiveConverter(Mode mode, Receiver receiver, Conversion conversion, QObject* parent)
    : QObject(parent), m_mode(mode), m_receiver(std::move(receiver)), m_conversion(std::move(conversion)) {
    if (m_mode == Mode::Threaded) {
        m_thread = std::thread([this] { run(); });
    }
}

LiveConverter::~LiveConverter() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_waiting.reset();
    }
    m_wake.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // A result posted before the thread stopped is removed with this object's posted events by ~QObject.
}

std::optional<LiveJob> LiveConverter::submit(LiveJob job) {
    m_newestSize = job.drawnSize;
    if (m_mode == Mode::Immediate) {
        if (std::optional<LiveResult> result = convert(job)) {
            m_receiver(std::move(*result));
        }
        return std::nullopt;
    }
    std::optional<LiveJob> replaced;
    {
        const std::lock_guard lock(m_mutex);
        if (m_waiting) {
            if (m_waiting->keep == ImageView::KeepView::No) {
                job.keep = ImageView::KeepView::No;
            }
            replaced = std::move(m_waiting);
        }
        m_waiting = std::move(job);
    }
    m_wake.notify_one();
    return replaced;
}

std::optional<cv::Size> LiveConverter::newestSize() const { return m_newestSize; }

void LiveConverter::run() {
    for (;;) {
        std::optional<LiveJob> job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || m_waiting.has_value(); });
            if (m_stopping) {
                return;
            }
            job = std::move(m_waiting);
            m_waiting.reset();
        }
        std::optional<LiveResult> result = convert(*job);
        job.reset(); // the frame is released here, not when the UI thread gets to the result
        if (!result) {
            continue;
        }
        const std::lock_guard lock(m_mutex);
        if (m_stopping) {
            return;
        }
        // Queued to this object, which lives on the UI thread: the receiver runs there, in posting order.
        QMetaObject::invokeMethod(
            this, [this, result = std::move(*result)]() mutable { m_receiver(std::move(result)); },
            Qt::QueuedConnection);
    }
}

std::optional<LiveResult> LiveConverter::convert(const LiveJob& job) const {
    try {
        LiveResult result = m_conversion(job);
        result.job.frame.release();
        return result;
    } catch (...) { // a frame that cannot be converted is not drawn; the next one may be
        return std::nullopt;
    }
}
