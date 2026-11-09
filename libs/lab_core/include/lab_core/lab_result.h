#pragma once

#include "lab_core/frame_processor.h"

#include <frame_io/frame.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace lab_core {

// One processed frame, as the engine hands it to the window and the sender.
struct LabResult {
    std::uint64_t generation = 0; // the source run it belongs to: results of an older run are dropped
    frame_io::Frame frame;
    ProcessorResult processed;
    std::string processorError; // empty unless process() threw or changed its frame
    std::chrono::nanoseconds processingTime{};
};

} // namespace lab_core
