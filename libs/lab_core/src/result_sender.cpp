#include "lab_core/result_sender.h"

#include <algorithm>
#include <utility>

namespace lab_core {

std::string channelFor(std::string_view imageName) {
    return std::string(kSourceChannel) + " " + std::string(imageName);
}

ResultSender::ResultSender(Mode mode, std::unique_ptr<visual_debug::ImageSender> sender)
    : m_mode(mode), m_sender(std::move(sender)) {
    if (m_mode == Mode::Threaded) {
        m_thread = std::thread([this] { run(); });
    }
}

ResultSender::~ResultSender() { stopThread(); }

void ResultSender::stopThread() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ResultSender::submit(LabResult result) {
    {
        const std::lock_guard lock(m_mutex);
        if (m_stats.paused || m_stopping) {
            return; // dropped, as meant: not "not sent"
        }
        if (m_waiting) {
            ++m_stats.notSent;
        }
        m_waiting = std::move(result);
    }
    m_wake.notify_all();
}

bool ResultSender::pump() {
    std::optional<LabResult> result;
    {
        const std::lock_guard lock(m_mutex);
        result.swap(m_waiting);
    }
    if (!result) {
        return false;
    }
    send(*result);
    return true;
}

void ResultSender::setPaused(bool paused) {
    std::optional<LabResult> dropped; // destroyed without the lock
    const std::lock_guard lock(m_mutex);
    m_stats.paused = paused;
    if (paused) {
        dropped.swap(m_waiting);
    }
}

void ResultSender::endChannels() {
    stopThread();
    std::optional<LabResult> dropped;
    {
        const std::lock_guard lock(m_mutex);
        dropped.swap(m_waiting);
    }
    for (const std::string& channel : m_channels) {
        m_sender->endLive(channel);
    }
    m_channels.clear();
}

ResultSender::Stats ResultSender::stats() const {
    const std::lock_guard lock(m_mutex);
    return m_stats;
}

void ResultSender::run() {
    for (;;) {
        std::optional<LabResult> result;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || m_waiting.has_value(); });
            if (m_stopping) {
                return;
            }
            result.swap(m_waiting);
        }
        send(*result);
    }
}

// Outside the lock: a send may take as long as the viewer's reply timeout.
void ResultSender::send(const LabResult& result) {
    visual_debug::ImageDescription description = result.frame.tags;
    description.overlays.insert(description.overlays.end(), result.processed.overlays.begin(),
                                result.processed.overlays.end());
    visual_debug::SendResult frameResult = visual_debug::SendResult::Delivered;
    sendLive(result.frame.image, kSourceChannel, description, &frameResult);
    for (const ProcessedImage& image : result.processed.images) {
        sendLive(image.image, channelFor(image.name), image.description, nullptr);
    }
    const std::lock_guard lock(m_mutex);
    ++m_stats.sent;
    m_stats.last = frameResult;
}

// Remembered before it is sent: a send that timed out may still have reached the viewer.
void ResultSender::sendLive(const cv::Mat& image, const std::string& channel,
                            const visual_debug::ImageDescription& description,
                            visual_debug::SendResult* result) {
    if (std::find(m_channels.begin(), m_channels.end(), channel) == m_channels.end()) {
        m_channels.push_back(channel);
    }
    const visual_debug::SendResult sent = m_sender->sendLive(image, channel, description);
    if (result) {
        *result = sent;
    }
}

} // namespace lab_core
