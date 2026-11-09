#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace frame_io {

// Runs `read` on a thread of its own, up to `depth` items ahead of take(), so decoding the next frames
// overlaps sending this one. take() blocks until an item is ready, and returns nothing once `read` has (or
// has thrown: then `onException`, if given, turns what was thrown into one last item). The destructor stops
// the thread without waiting for `read` to run dry; a read already running finishes.
template <typename T> class ReadAhead {
public:
    ReadAhead(std::function<std::optional<T>()> read, std::size_t depth,
              std::function<std::optional<T>(std::string what)> onException = {})
        : m_read(std::move(read)), m_onException(std::move(onException)), m_depth(depth == 0 ? 1 : depth),
          m_thread([this] { readUntilDone(); }) {}

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

    // Called on the reading thread after each item is ready and when the stream ends, so a caller can wait on
    // a condition of its own for a frame and for anything else at once. Set it before the first item is ready
    // to be called for every one.
    void setWake(std::function<void()> wake) {
        const std::lock_guard lock(m_mutex);
        m_wake = std::move(wake);
    }

    // Whether take() would return at once: an item waits, or the stream has ended.
    bool ready() const {
        const std::lock_guard lock(m_mutex);
        return !m_items.empty() || m_ended;
    }

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
            // A reader that throws ends the stream, after the handler's item if it gives one: the caller sees
            // an error or the end, never a terminated program.
            std::optional<T> item;
            bool threw = false;
            try {
                item = m_read();
            } catch (const std::exception& exception) {
                threw = true;
                item = lastItem(exception.what());
            } catch (...) {
                threw = true;
                item = lastItem("unknown exception");
            }
            std::function<void()> wake;
            const bool ended = threw || !item;
            {
                const std::lock_guard lock(m_mutex);
                if (item) {
                    m_items.push_back(std::move(*item));
                }
                m_ended = ended;
                wake = m_wake;
            }
            m_changed.notify_all();
            if (wake) {
                wake(); // outside the lock: it may ask ready()
            }
            if (ended) {
                return;
            }
        }
    }

    // The handler's item for what the reader threw; nothing without a handler, or if the handler throws too.
    std::optional<T> lastItem(const char* what) {
        if (!m_onException) {
            return std::nullopt;
        }
        try {
            return m_onException(what);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::function<std::optional<T>()> m_read;
    std::function<std::optional<T>(std::string what)> m_onException;
    std::size_t m_depth;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<T> m_items;
    bool m_ended = false;
    bool m_stopping = false;
    std::function<void()> m_wake;
    std::thread m_thread; // last, so it starts after everything it uses
};

} // namespace frame_io
