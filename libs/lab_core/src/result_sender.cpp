#include "lab_core/result_sender.h"

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

ResultSender::~ResultSender() {
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
    const visual_debug::SendResult frameResult =
        m_sender->sendLive(result.frame.image, kSourceChannel, description);
    for (const ProcessedImage& image : result.processed.images) {
        m_sender->sendLive(image.image, channelFor(image.name), image.description);
    }
    const std::lock_guard lock(m_mutex);
    ++m_stats.sent;
    m_stats.last = frameResult;
}

} // namespace lab_core
