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

class RecordingSender : public visual_debug::ImageSender {
public:
    struct Call {
        std::string name;
        bool live = false;
        cv::Mat image;
        visual_debug::ImageDescription description{};
    };

    explicit RecordingSender(
        std::function<visual_debug::SendResult(std::string_view)> answer =
            [](std::string_view) { return visual_debug::SendResult::Delivered; },
        Latch* holdLive = nullptr)
        : m_answer(std::move(answer)), m_holdLive(holdLive) {}

    visual_debug::SendResult send(const cv::Mat& image, std::string_view name) override {
        return record({std::string(name), false, image, {}});
    }

    visual_debug::SendResult sendLive(const cv::Mat& frame, std::string_view channel) override {
        return sendLive(frame, channel, {});
    }

    visual_debug::SendResult sendLive(const cv::Mat& frame, std::string_view channel,
                                      const visual_debug::ImageDescription& description) override {
        if (m_holdLive) {
            m_holdLive->hold();
        }
        return record({std::string(channel), true, frame, description});
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
    visual_debug::SendResult record(Call call) {
        const visual_debug::SendResult result = m_answer(call.name);
        const std::lock_guard lock(m_mutex);
        m_calls.push_back(std::move(call));
        m_called.notify_all();
        return result;
    }

    std::function<visual_debug::SendResult(std::string_view)> m_answer;
    Latch* m_holdLive;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_called;
    std::vector<Call> m_calls;
};

} // namespace test_support
