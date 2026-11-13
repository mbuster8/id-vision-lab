#pragma once

#include <visual_debug/image_description.h>
#include <visual_debug/image_sender.h>

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lab_core {

// The lab's own connection to the viewer, beside the stream's: it pings the viewer to say whether it is
// there, and sends single frames into its history. One ImageSender, used on one thread of its own (a ZeroMQ
// REQ socket serves one thread), made again for each endpoint. Free of Qt.
class ViewerLink {
public:
    // Threaded: a thread of its own. Immediate: pump() does the work, for the tests.
    enum class Mode { Threaded, Immediate };
    using Clock = std::chrono::steady_clock;
    using MakeSender = std::function<std::unique_ptr<visual_debug::ImageSender>(const std::string& endpoint)>;
    struct Hooks {
        std::function<void()> changed;          // the state changed; on the link's thread, no lock held
        std::function<Clock::time_point()> now; // null: Clock::now
    };

    static constexpr std::chrono::seconds kPingInterval{2};
    // An older viewer logs each ping as a rejected request: it is asked far less often.
    static constexpr std::chrono::seconds kOlderViewerInterval{60};
    static constexpr std::size_t kMaxStills = 4;

    ViewerLink(Mode mode, MakeSender make, Hooks hooks);
    ViewerLink(const ViewerLink&) = delete;
    ViewerLink& operator=(const ViewerLink&) = delete;
    // After stop() and idle(), joins at once; otherwise waits for a request in progress.
    ~ViewerLink();

    // Pings it at once, then every kPingInterval; "" stops pinging. A new endpoint forgets what was known of
    // the last one, refuses (and counts) the stills still waiting for it, and drops its late answers.
    void setEndpoint(std::string endpoint);
    // Sent in order, as an ordinary image into the viewer's history. At most kMaxStills wait; a further one,
    // or one with no endpoint, is refused and counted. Returns the still's number (1, 2, …), by which
    // State::lastStillNumber says when it has ended. `refused`, when given, is set to why it was refused at
    // once, or emptied: an older still's answer may replace lastStillNumber before the caller reads the
    // state.
    std::uint64_t sendStill(cv::Mat image, std::string name, visual_debug::ImageDescription description,
                            std::string* refused = nullptr);
    // Drops the stills waiting and stops pinging; returns at once. A request in progress finishes.
    void stop();
    // Nothing in progress, nothing waiting and no ping due: after stop(), the link can be destroyed at once.
    bool idle() const;

    struct State {
        std::string endpoint;
        std::optional<visual_debug::ViewerState> viewer; // nothing until a ping has answered
        std::optional<visual_debug::SendResult> lastStill;
        std::string lastStillName;
        std::uint64_t stillsRefused = 0;
        // The still that ended last: answered (lastStill its result), refused, or dropped by a new endpoint
        // or stop() (lastStill empty, lastStillReason why, in fixed words).
        std::uint64_t lastStillNumber = 0;
        std::string lastStillReason;
    };
    State state() const;

    // Immediate mode: a waiting still, else a ping that is due. Whether there was one.
    bool pump();

private:
    struct Still {
        cv::Mat image;
        std::string name;
        visual_debug::ImageDescription description;
        std::uint64_t number = 0;
    };
    // One request, taken under the lock and made without it.
    struct Work {
        std::optional<Still> still; // nothing: a ping
        std::string endpoint;
        std::uint64_t epoch = 0;
    };

    void run();
    std::optional<Work> takeWork(); // with the lock held
    void doWork(const Work& work);
    // With the lock held: the stills waiting and the one in progress end, unanswered; true if there were any.
    bool dropStills(std::deque<Still>& dropped);
    Clock::time_point now() const;

    const Mode m_mode;
    MakeSender m_make;
    Hooks m_hooks;

    // The link's thread's own.
    std::unique_ptr<visual_debug::ImageSender> m_sender;
    std::uint64_t m_senderEpoch = 0;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::uint64_t m_epoch = 0; // the endpoint's: a new one starts a new epoch
    std::deque<Still> m_stills;
    std::uint64_t m_stillsNumbered = 0;
    std::optional<std::uint64_t> m_stillInProgress; // its number
    std::optional<Clock::time_point> m_nextPing;
    bool m_busy = false;
    bool m_stopped = false; // until the next setEndpoint()
    bool m_quit = false;
    State m_state;

    std::thread m_thread; // last, so it starts after everything it uses
};

} // namespace lab_core
