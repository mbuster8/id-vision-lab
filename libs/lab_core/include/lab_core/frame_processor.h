#pragma once

#include <frame_io/frame.h>
#include <visual_debug/image_description.h>
#include <visual_debug/overlay.h>

#include <opencv2/core/mat.hpp>

#include <string>
#include <vector>

namespace lab_core {

// A debug picture a processor made from a frame, such as its edges.
struct ProcessedImage {
    std::string name; // the Show picker's entry, and the live channel "lab <name>"
    cv::Mat image;
    visual_debug::ImageDescription description{}; // its tags and its own overlays
};

// What a processor found in one frame.
struct ProcessorResult {
    std::vector<visual_debug::Overlay> overlays{}; // over the camera frame: on the canvas and sent with it
    std::vector<ProcessedImage> images{};          // debug pictures
    std::vector<std::string> findings{}; // short lines for the results panel, such as "edges: 4.2 %"
};

// The Document Lab's hook for vision work: one processor at a time, called on the processing thread only, one
// frame at a time, so it may keep state between frames.
class FrameProcessor {
public:
    virtual ~FrameProcessor() = default;
    // `frame.image` must not be written to, not even through a copy of the Mat's header: the recorder,
    // capture, the canvas and the sender get the same pixels after this returns.
    virtual ProcessorResult process(const frame_io::Frame& frame) = 0;
    // A new source or run, a restart, a step back, a loop, or another processor: forget earlier frames.
    virtual void reset() {}
};

} // namespace lab_core
