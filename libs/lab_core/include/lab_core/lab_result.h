#pragma once

#include "lab_core/focus_control.h"
#include "lab_core/frame_processor.h"

#include <frame_io/frame.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace lab_core {

// One processed frame, as the engine hands it to the window and the sender.
struct LabResult {
    std::uint64_t generation = 0; // the source run it belongs to: results of an older run are dropped
    frame_io::Frame frame;
    ProcessorResult processed;
    std::string processorError; // empty unless process() threw or changed its frame
    std::chrono::nanoseconds processingTime{};
    std::optional<FocusStatus> focus{}; // a camera run's: what the lab's focus is doing
};

} // namespace lab_core
