#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace live_sender {

// Runs `read` on a thread of its own, up to `depth` items ahead of take(), so decoding the next frames
// overlaps sending this one. take() blocks until an item is ready, and returns nothing once `read` has (or
// has thrown). The destructor stops the thread without waiting for `read` to run dry; a read already running
// finishes.
template <typename T> class ReadAhead {
public:
    ReadAhead(std::function<std::optional<T>()> read, std::size_t depth)
        : m_read(std::move(read)), m_depth(depth == 0 ? 1 : depth), m_thread([this] { readUntilDone(); }) {}

    ~ReadAhead() {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_changed.notify_all();
        m_thread.join();
    }

    ReadAhead(const ReadAhead&) = delete;
    ReadAhead& operator=(const ReadAhead&) = delete;

    std::optional<T> take() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return !m_items.empty() || m_ended; });
        if (m_items.empty()) {
            return std::nullopt;
        }
        T item = std::move(m_items.front());
        m_items.pop_front();
        lock.unlock();
        m_changed.notify_all(); // room for the next read
        return item;
    }

private:
    // Waits for room before reading, so at most `depth` items are ever read and not taken.
    void readUntilDone() {
        while (true) {
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [this] { return m_items.size() < m_depth || m_stopping; });
                if (m_stopping) {
                    return;
                }
            }
            std::optional<T> item;
            try {
                item = m_read();
            } catch (const std::exception&) {
                // A reader that fails ends the stream: the caller sees its end, not a terminated program.
            }
            {
                const std::lock_guard lock(m_mutex);
                if (item) {
                    m_items.push_back(std::move(*item));
                } else {
                    m_ended = true;
                }
            }
            m_changed.notify_all();
            if (!item) {
                return;
            }
        }
    }

    std::function<std::optional<T>()> m_read;
    std::size_t m_depth;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<T> m_items;
    bool m_ended = false;
    bool m_stopping = false;
    std::thread m_thread; // last, so it starts after everything it uses
};

} // namespace live_sender
