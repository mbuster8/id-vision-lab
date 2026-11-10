#pragma once

#include "visual_debug/image_sink.h"
#include "visual_debug/server.h"

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace test_support {

// Keeps the reason of every request the server turned away. Thread-safe.
class RejectionRecordingSink : public visual_debug::ImageSink {
public:
    void onRequestRejected(std::string_view reason) override {
        std::lock_guard lock(m_mutex);
        m_rejections.emplace_back(reason);
    }

    std::vector<std::string> rejections() const {
        std::lock_guard lock(m_mutex);
        return m_rejections;
    }

    void onLiveEnded(std::string_view name) override {
        std::lock_guard lock(m_mutex);
        m_ended.emplace_back(name);
    }

    // The live channels ended by their senders, in order.
    std::vector<std::string> ended() const {
        std::lock_guard lock(m_mutex);
        return m_ended;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_rejections;
    std::vector<std::string> m_ended;
};

// Thread-safe sink that keeps every image it receives.
class CollectingSink : public RejectionRecordingSink {
public:
    visual_debug::Acceptance onImageReceived(visual_debug::ImageMessage message) override {
        {
            std::lock_guard lock(m_mutex);
            m_messages.push_back(std::move(message));
        }
        m_changed.notify_all();
        return visual_debug::Acceptance::Taken;
    }

    // False if fewer than `count` images have arrived when the timeout expires.
    bool waitForCount(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&] { return m_messages.size() >= count; });
    }

    std::vector<visual_debug::ImageMessage> messages() const {
        std::lock_guard lock(m_mutex);
        return m_messages;
    }

    std::size_t count() const {
        std::lock_guard lock(m_mutex);
        return m_messages.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<visual_debug::ImageMessage> m_messages;
};

// A viewer that is always too busy to take another image.
class BusySink : public RejectionRecordingSink {
public:
    visual_debug::Acceptance onImageReceived(visual_debug::ImageMessage) override {
        return visual_debug::Acceptance::Busy;
    }
};

// A viewer whose live channels are all taken.
class NoLiveChannelLeftSink : public RejectionRecordingSink {
public:
    visual_debug::Acceptance onImageReceived(visual_debug::ImageMessage) override {
        return visual_debug::Acceptance::TooManyLiveChannels;
    }
};

inline void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

inline void unsetEnvironment(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// The variable's value, or nothing when it is not set. MSVC deprecates getenv (warning C4996) for _dupenv_s.
inline std::optional<std::string> environmentValue(const char* name) {
#ifdef _WIN32
    char* copy = nullptr;
    std::size_t length = 0;
    std::optional<std::string> value;
    if (_dupenv_s(&copy, &length, name) == 0 && copy != nullptr) {
        value = copy;
    }
    std::free(copy);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

inline bool isAscii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// A loopback endpoint that was free a moment ago and has nothing listening on it now.
inline std::string unusedEndpoint() {
    CollectingSink sink;
    visual_debug::Server server(sink, {.endpoint = "tcp://127.0.0.1:*"});
    if (server.start()) {
        return {};
    }
    auto endpoint = server.boundEndpoint();
    server.stop();
    return endpoint;
}

// Something on a loopback port that answers every request with the same reply, as an older viewer or another
// program would, on a thread of its own until destroyed.
class ScriptedPeer {
public:
    explicit ScriptedPeer(std::string reply)
        : m_reply(std::move(reply)), m_socket(m_context, zmq::socket_type::rep) {
        m_socket.set(zmq::sockopt::linger, 0);
        m_socket.bind("tcp://127.0.0.1:*");
        m_endpoint = m_socket.get(zmq::sockopt::last_endpoint);
        m_thread = std::thread([this] { serve(); });
    }
    ~ScriptedPeer() {
        m_stop = true;
        m_thread.join();
    }
    ScriptedPeer(const ScriptedPeer&) = delete;
    ScriptedPeer& operator=(const ScriptedPeer&) = delete;

    std::string endpoint() const { return m_endpoint; }

private:
    void serve() {
        while (!m_stop) {
            zmq::pollitem_t item{m_socket.handle(), 0, ZMQ_POLLIN, 0};
            zmq::poll(&item, 1, std::chrono::milliseconds(20));
            if ((item.revents & ZMQ_POLLIN) == 0) {
                continue;
            }
            zmq::message_t frame;
            do {
                (void)m_socket.recv(frame);
            } while (frame.more());
            m_socket.send(zmq::buffer(m_reply), zmq::send_flags::none);
        }
    }

    std::string m_reply;
    zmq::context_t m_context;
    zmq::socket_t m_socket;
    std::string m_endpoint;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

// Sends raw frames from a plain REQ socket, as a misbehaving sender would, and returns the parsed reply,
// or nothing if no reply arrives within a second.
inline std::optional<nlohmann::json> rawRequest(const std::string& endpoint,
                                                const std::vector<std::string>& frames) {
    zmq::context_t context;
    zmq::socket_t socket(context, zmq::socket_type::req);
    socket.set(zmq::sockopt::linger, 0);
    socket.connect(endpoint);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto flags = i + 1 < frames.size() ? zmq::send_flags::sndmore : zmq::send_flags::none;
        if (!socket.send(zmq::buffer(frames[i]), flags)) {
            return std::nullopt;
        }
    }
    zmq::pollitem_t item{socket.handle(), 0, ZMQ_POLLIN, 0};
    zmq::poll(&item, 1, std::chrono::milliseconds(1000));
    if ((item.revents & ZMQ_POLLIN) == 0) {
        return std::nullopt;
    }
    zmq::message_t reply;
    if (!socket.recv(reply)) {
        return std::nullopt;
    }
    return nlohmann::json::parse(reply.to_string_view(), nullptr, /*allow_exceptions=*/false);
}

} // namespace test_support
