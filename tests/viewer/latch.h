#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace test_support {

// Holds a live conversion on the converter's thread until the test releases it, so what waits in the slot
// meanwhile is known without timing. A hold gives up after ten seconds, so a test that fails before releasing
// it cannot leave the converter's destructor waiting for ever.
class Latch {
public:
    void hold() {
        std::unique_lock lock(m_mutex);
        m_entered = true;
        m_changed.notify_all();
        m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_released; });
    }
    bool waitUntilEntered() {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_entered; });
    }
    void release() {
        const std::lock_guard lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_entered = false;
    bool m_released = false;
};

} // namespace test_support
