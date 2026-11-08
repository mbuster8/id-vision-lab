#pragma once

#include "image_view.h"

#include "viewer_core/display_settings.h"
#include "viewer_core/histogram.h"
#include "viewer_core/image_tags.h"

#include <QImage>
#include <QObject>
#include <QSize>

#include <opencv2/core/mat.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// One picture of the live frame in view to make: everything the conversion needs, copied or shared read-only,
// so that it never reads the window's state from another thread.
struct LiveJob {
    std::uint64_t viewCounter = 0; // what was in view when it was made; a result for another view is dropped
    std::string channel;
    std::uint64_t frameNumber = 0; // the channel's LiveChannel::shownNumber
    cv::Mat frame;                 // shares the channel's pixels, which nothing changes in place
    viewer_core::DisplaySettings settings{};
    viewer_core::ImageTags tags{};
    cv::Size drawnSize{}; // what the picture is made at
    bool showClipping = false;
    bool histogram = false; // the strip is shown: count the frame's histogram too
    ImageView::KeepView keep = ImageView::KeepView::IfSameSize;
};

struct LiveResult {
    LiveJob job;                                        // its frame released: the window needs only its size
    QSize imageSize;                                    // the frame's
    QImage picture;                                     // shares the converted pixels
    std::optional<viewer_core::Histogram> histogram;    // when the job asked for it
    std::optional<viewer_core::RangeMarkers> markers{}; // with it
};

// The picture at the job's drawn size, and the histogram when asked: viewer_core's free functions only, which
// keep no state, so this runs on any thread.
LiveResult convertLiveFrame(const LiveJob& job);

// Converts the live frame in view off the UI thread: one worker thread, one waiting job. A job submitted
// while the worker is busy replaces the one waiting, so the worker always converts the newest frame and the
// UI never waits for it. Results reach the receiver on the thread the converter lives in, in submission
// order. Immediate mode converts on the calling thread before submit returns, for tests that must not wait.
class LiveConverter : public QObject {
    Q_OBJECT

public:
    enum class Mode { Threaded, Immediate };
    using Conversion = std::function<LiveResult(const LiveJob&)>;
    using Receiver = std::function<void(LiveResult)>;

    LiveConverter(Mode mode, Receiver receiver, Conversion conversion = convertLiveFrame,
                  QObject* parent = nullptr);
    // Waits for a conversion in progress, then stops the thread; nothing reaches the receiver afterwards.
    ~LiveConverter() override;

    LiveConverter(const LiveConverter&) = delete;
    LiveConverter& operator=(const LiveConverter&) = delete;

    // The job it replaced, if one was still waiting, for the caller to count its frame as skipped. A fit
    // asked by the replaced job (KeepView::No) carries into `job`. A conversion that throws delivers nothing.
    std::optional<LiveJob> submit(LiveJob job);
    // The drawn size of the newest job submitted: what the view will show next, for deciding on a redraw.
    std::optional<cv::Size> newestSize() const;

private:
    void run();
    std::optional<LiveResult> convert(const LiveJob& job) const; // nothing when the conversion throws

    Mode m_mode;
    Receiver m_receiver;
    Conversion m_conversion;
    std::optional<cv::Size> m_newestSize; // the thread that submits only

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<LiveJob> m_waiting; // guarded by m_mutex
    bool m_stopping = false;          // guarded by m_mutex
    std::thread m_thread;             // last, so it starts once the rest exists
};
