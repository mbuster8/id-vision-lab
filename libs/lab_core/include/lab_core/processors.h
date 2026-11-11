#pragma once

#include "lab_core/frame_processor.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace lab_core {

// The processors: None; Edges (a debug picture of the frame's edges, and their share of the frame); Outline
// (the document's outline as a polygon over the frame, held over a frame or two in which it is lost);
// Document (DocAligner LC100 finds the page, six quality gates judge it, and a hint tells the user what to
// change: document_processor.h).
enum class ProcessorKind { None, Edges, Outline, Document };

// The processor picker's entry: "None", "Edges", "Outline", "Document".
std::string_view nameOf(ProcessorKind kind);

// What a processor may need from the program that makes it.
struct ProcessorSettings {
    std::filesystem::path modelFolder{}; // where the Document processor finds kLc100ModelFile
};

// DocAligner LC100's file in the model folder.
inline constexpr const char* kLc100ModelFile = "lcnet100_h_e_bifpn_256_fp32.onnx";

std::unique_ptr<FrameProcessor> makeProcessor(ProcessorKind kind, const ProcessorSettings& settings = {});

} // namespace lab_core
