#pragma once

#include <chrono>
#include <optional>

namespace frame_io {

// When each frame of a replay is shown. A replay never skips a frame: one that is ready late (the processing
// was slow, the disk stalled) is shown when ready, and the schedule goes on from there instead of rushing to
// catch up. So a replay plays at its own pace or slower, never faster, and every frame is processed.
class ReplayPacer {
public:
    using Clock = std::chrono::steady_clock;

    // Forgets the schedule: after a seek, a pause or a restart from the beginning.
    void restart();

    // The frame at `at` (its time in the replay) ready at `ready`: due the previous frame's due time plus the
    // frames' gap, or `ready` if that is later, from which the schedule then continues. The first frame after
    // restart() is due when ready. A frame whose time goes backwards gets no gap.
    Clock::time_point due(std::chrono::nanoseconds at, Clock::time_point ready);

private:
    struct Previous {
        std::chrono::nanoseconds at;
        Clock::time_point due;
    };
    std::optional<Previous> m_previous;
};

} // namespace frame_io
