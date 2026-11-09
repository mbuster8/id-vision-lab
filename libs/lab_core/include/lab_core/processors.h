#pragma once

#include "lab_core/frame_processor.h"

#include <memory>
#include <string_view>

namespace lab_core {

// A2's processors: None; Edges (a debug picture of the frame's edges, and their share of the frame); Outline
// (the document's outline as a polygon over the frame, held over a frame or two in which it is lost).
enum class ProcessorKind { None, Edges, Outline };

// The processor picker's entry: "None", "Edges", "Outline".
std::string_view nameOf(ProcessorKind kind);

std::unique_ptr<FrameProcessor> makeProcessor(ProcessorKind kind);

} // namespace lab_core
