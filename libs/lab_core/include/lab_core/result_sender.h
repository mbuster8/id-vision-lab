#pragma once

#include "lab_core/lab_result.h"

#include <visual_debug/image_sender.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lab_core {

// The live channel the lab's frames go out on, and the prefix of its pictures' channels. The names do not
// change with the source.
inline constexpr char kSourceChannel[] = "lab";

// "lab <name>": the channel a processor's picture goes out on.
std::string channelFor(std::string_view imageName);

// Sends each result to the viewer as live frames: the frame as "lab", carrying its tags and the processor's
// overlays, then each picture as "lab <name>". One result at a time; one not yet sent when a newer one comes
// is replaced whole, so the viewer's channels show the same frame, and a slow or absent viewer never slows
// the processing. Threaded, it sends on a thread of its own, the only one that touches the ImageSender;
// Immediate, pump() sends, for the tests.
class ResultSender {
public:
    enum class Mode { Threaded, Immediate };

    ResultSender(Mode mode, std::unique_ptr<visual_debug::ImageSender> sender);
    ResultSender(const ResultSender&) = delete;
    ResultSender& operator=(const ResultSender&) = delete;
    // Waits for a send in progress; a result still waiting is not sent.
    ~ResultSender();

    // Hands over a result to send; one still waiting is replaced, and counted as not sent.
    void submit(LabResult result);

    // Immediate mode: sends the waiting result, if any. Whether there was one.
    bool pump();

    // While paused, a result handed over is dropped, and one waiting too, without being counted as not sent:
    // ⏸ keeps the viewer's channels showing the last frame sent.
    void setPaused(bool paused);
    // Stops the sending thread, after a send in progress (a result waiting is not sent), then ends every
    // channel it sent on, once each, in the order first sent, on the calling thread. Best effort: the
    // client's back-off makes the ends return at once after one has timed out. Nothing is sent after it.
    void endChannels();

    struct Stats {
        std::uint64_t sent = 0;                       // results that went out
        std::uint64_t notSent = 0;                    // results replaced by a newer one before they went out
        std::optional<visual_debug::SendResult> last; // the frame's channel's result for the last one
        bool paused = false;
    };
    Stats stats() const;

private:
    void run();
    void stopThread();
    void send(const LabResult& result);
    void sendLive(const cv::Mat& image, const std::string& channel,
                  const visual_debug::ImageDescription& description, visual_debug::SendResult* result);

    Mode m_mode;
    std::unique_ptr<visual_debug::ImageSender> m_sender;
    // The channels sent on, in the order first sent; only the sending thread, or pump()'s caller, touches it
    // until endChannels() has stopped that thread.
    std::vector<std::string> m_channels;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<LabResult> m_waiting;
    bool m_stopping = false;
    Stats m_stats;
    std::thread m_thread; // last, so it starts after everything it uses
};

} // namespace lab_core
