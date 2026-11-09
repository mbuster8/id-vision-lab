#include "frame_io/replay_pacer.h"

#include <algorithm>

namespace frame_io {

void ReplayPacer::restart() { m_previous.reset(); }

ReplayPacer::Clock::time_point ReplayPacer::due(std::chrono::nanoseconds at, Clock::time_point ready) {
    Clock::time_point when = ready;
    if (m_previous) {
        const std::chrono::nanoseconds gap = std::max(at - m_previous->at, std::chrono::nanoseconds{0});
        when = std::max(m_previous->due + std::chrono::duration_cast<Clock::duration>(gap), ready);
    }
    m_previous = Previous{.at = at, .due = when};
    return when;
}

} // namespace frame_io
