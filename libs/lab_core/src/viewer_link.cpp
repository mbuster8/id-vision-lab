#include "lab_core/viewer_link.h"

#include <algorithm>
#include <utility>

namespace lab_core {

ViewerLink::ViewerLink(Mode mode, MakeSender make, Hooks hooks)
    : m_mode(mode), m_make(std::move(make)), m_hooks(std::move(hooks)) {
    if (m_mode == Mode::Threaded) {
        m_thread = std::thread([this] { run(); });
    }
}

ViewerLink::~ViewerLink() {
    {
        const std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ViewerLink::setEndpoint(std::string endpoint) {
    std::deque<Still> refused; // destroyed without the lock
    {
        const std::lock_guard lock(m_mutex);
        if (endpoint == m_state.endpoint && !m_stopped) {
            return; // the same viewer, still being asked
        }
        m_stopped = false;
        ++m_epoch;
        refused.swap(m_stills);
        m_state.stillsRefused += refused.size();
        m_state.endpoint = std::move(endpoint);
        m_state.viewer.reset();
        m_state.lastStill.reset();
        m_state.lastStillName.clear();
        m_nextPing = m_state.endpoint.empty() ? std::nullopt : std::optional(now());
    }
    m_wake.notify_all();
}

void ViewerLink::sendStill(cv::Mat image, std::string name, visual_debug::ImageDescription description) {
    {
        const std::lock_guard lock(m_mutex);
        if (m_state.endpoint.empty() || m_stills.size() >= kMaxStills) {
            ++m_state.stillsRefused;
            return;
        }
        m_stills.push_back({std::move(image), std::move(name), std::move(description)});
    }
    m_wake.notify_all();
}

void ViewerLink::stop() {
    std::deque<Still> dropped;
    {
        const std::lock_guard lock(m_mutex);
        ++m_epoch; // an answer in progress is dropped, and schedules nothing
        m_stopped = true;
        dropped.swap(m_stills);
        m_nextPing.reset();
    }
    m_wake.notify_all();
}

bool ViewerLink::idle() const {
    const std::lock_guard lock(m_mutex);
    return !m_busy && m_stills.empty() && !m_nextPing;
}

ViewerLink::State ViewerLink::state() const {
    const std::lock_guard lock(m_mutex);
    return m_state;
}

bool ViewerLink::pump() {
    if (m_mode != Mode::Immediate) {
        return false;
    }
    std::optional<Work> work;
    {
        const std::lock_guard lock(m_mutex);
        work = takeWork();
    }
    if (!work) {
        return false;
    }
    doWork(*work);
    return true;
}

void ViewerLink::run() {
    for (;;) {
        std::optional<Work> work;
        {
            std::unique_lock lock(m_mutex);
            for (;;) {
                if (m_quit) {
                    return;
                }
                work = takeWork();
                if (work) {
                    break;
                }
                if (m_nextPing) {
                    m_wake.wait_for(lock, std::max(*m_nextPing - now(), Clock::duration::zero()));
                } else {
                    m_wake.wait(lock);
                }
            }
        }
        doWork(*work);
    }
}

// A still waiting, else a ping that is due; marks the link busy.
std::optional<ViewerLink::Work> ViewerLink::takeWork() {
    Work work{.still = std::nullopt, .endpoint = m_state.endpoint, .epoch = m_epoch};
    if (!m_stills.empty()) {
        work.still = std::move(m_stills.front());
        m_stills.pop_front();
    } else if (!m_nextPing || now() < *m_nextPing) {
        return std::nullopt;
    } else {
        m_nextPing.reset(); // set again by the answer
    }
    m_busy = true;
    return work;
}

// Without the lock: a request may take as long as the viewer's reply timeout. Its answer counts only if the
// endpoint is still the one it was made for.
void ViewerLink::doWork(const Work& work) {
    std::optional<visual_debug::ViewerState> viewer;
    std::optional<visual_debug::SendResult> sent;
    try {
        if (!m_sender || m_senderEpoch != work.epoch) {
            m_sender.reset();
            m_sender = m_make ? m_make(work.endpoint) : nullptr;
            m_senderEpoch = work.epoch;
        }
        if (work.still) {
            sent = m_sender ? m_sender->send(work.still->image, work.still->name, work.still->description)
                            : visual_debug::SendResult::ViewerUnavailable;
        } else {
            viewer = m_sender ? m_sender->ping() : visual_debug::ViewerState::Unavailable;
        }
    } catch (...) {
        // A sender that throws (none of the project's does) is a viewer that is not there.
        m_sender.reset();
        viewer = work.still ? std::nullopt : std::optional(visual_debug::ViewerState::Unavailable);
        sent = work.still ? std::optional(visual_debug::SendResult::ViewerUnavailable) : std::nullopt;
    }
    bool changed = false;
    {
        const std::lock_guard lock(m_mutex);
        m_busy = false;
        if (work.epoch == m_epoch) {
            changed = true;
            if (work.still) {
                m_state.lastStill = sent;
                m_state.lastStillName = work.still->name;
            } else {
                m_state.viewer = viewer;
                m_nextPing = now() + (viewer == visual_debug::ViewerState::PredatesPing
                                          ? std::chrono::duration_cast<Clock::duration>(kOlderViewerInterval)
                                          : std::chrono::duration_cast<Clock::duration>(kPingInterval));
            }
        }
    }
    m_wake.notify_all(); // a waiting idle() or stop() may care
    if (changed && m_hooks.changed) {
        m_hooks.changed();
    }
}

ViewerLink::Clock::time_point ViewerLink::now() const { return m_hooks.now ? m_hooks.now() : Clock::now(); }

} // namespace lab_core
