#pragma once

// An ImageSender for the lab's tests: records every call, answers each channel with the result it is given,
// and can hold its live sends on a latch so a test knows, without timing, what waits meanwhile. Safe to call
// from a sending thread while the test reads it.

#include "latch.h"

#include <visual_debug/image_sender.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support {

// The calls of several senders, in the order they were made, as "<kind> <name>" ("live lab", "end lab"), for
// tests in which an old sender and a new one send from threads of their own.
class SendLog {
public:
    void add(std::string line) {
        const std::lock_guard lock(m_mutex);
        m_lines.push_back(std::move(line));
    }
    std::vector<std::string> lines() const {
        const std::lock_guard lock(m_mutex);
        return m_lines;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

class RecordingSender : public visual_debug::ImageSender {
public:
    enum class Kind { Image, Live, Ping, End };

    struct Call {
        std::string name; // empty for a ping
        Kind kind = Kind::Live;
        cv::Mat image{};
        visual_debug::ImageDescription description{};
    };

    explicit RecordingSender(
        std::function<visual_debug::SendResult(std::string_view)> answer =
            [](std::string_view) { return visual_debug::SendResult::Delivered; },
        Latch* holdLive = nullptr)
        : m_answer(std::move(answer)), m_holdLive(holdLive) {}

    // What ping() answers; Listening when empty.
    std::function<visual_debug::ViewerState()> pingAnswer{};
    // Where each call is also written, if anywhere.
    SendLog* log = nullptr;

    visual_debug::SendResult send(const cv::Mat& image, std::string_view name) override {
        return send(image, name, {});
    }

    visual_debug::SendResult send(const cv::Mat& image, std::string_view name,
                                  const visual_debug::ImageDescription& description) override {
        return answered({std::string(name), Kind::Image, image, description});
    }

    visual_debug::SendResult sendLive(const cv::Mat& frame, std::string_view channel) override {
        return sendLive(frame, channel, {});
    }

    visual_debug::SendResult sendLive(const cv::Mat& frame, std::string_view channel,
                                      const visual_debug::ImageDescription& description) override {
        if (m_holdLive) {
            m_holdLive->hold();
        }
        return answered({std::string(channel), Kind::Live, frame, description});
    }

    visual_debug::ViewerState ping() override {
        record({std::string(), Kind::Ping, {}, {}});
        return pingAnswer ? pingAnswer() : visual_debug::ViewerState::Listening;
    }

    visual_debug::SendResult endLive(std::string_view channel) override {
        return answered({std::string(channel), Kind::End, {}, {}});
    }

    std::vector<Call> calls() const {
        const std::lock_guard lock(m_mutex);
        return m_calls;
    }

    // Whether `count` calls were made within ten seconds.
    bool waitForCalls(std::size_t count) const {
        std::unique_lock lock(m_mutex);
        return m_called.wait_for(lock, std::chrono::seconds(10), [&] { return m_calls.size() >= count; });
    }

private:
    static const char* nameOf(Kind kind) {
        switch (kind) {
        case Kind::Image:
            return "image";
        case Kind::Live:
            return "live";
        case Kind::Ping:
            return "ping";
        case Kind::End:
            return "end";
        }
        return "?";
    }

    visual_debug::SendResult answered(Call call) {
        const visual_debug::SendResult result = m_answer(call.name);
        record(std::move(call));
        return result;
    }

    void record(Call call) {
        if (log) {
            log->add(std::string(nameOf(call.kind)) + " " + call.name);
        }
        const std::lock_guard lock(m_mutex);
        m_calls.push_back(std::move(call));
        m_called.notify_all();
    }

    std::function<visual_debug::SendResult(std::string_view)> m_answer;
    Latch* m_holdLive;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_called;
    std::vector<Call> m_calls;
};

} // namespace test_support
