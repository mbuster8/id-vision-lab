#pragma once

#include <doc_vision/guidance.h>
#include <frame_io/frame.h>
#include <visual_debug/image_description.h>
#include <visual_debug/overlay.h>

#include <opencv2/core/mat.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lab_core {

// A debug picture a processor made from a frame, such as its edges.
struct ProcessedImage {
    std::string name; // the Show picker's entry, and the live channel "lab <name>"
    cv::Mat image;
    visual_debug::ImageDescription description{}; // its tags and its own overlays
};

// One row of the results panel: a name, its value, and whether the value is good news, bad news or neither
// (the panel colours it green or red).
struct Finding {
    enum class Tone { Plain, Good, Bad };
    std::string name;
    std::string value;
    Tone tone = Tone::Plain;

    bool operator==(const Finding&) const = default;
};

// "Name: value", or the name alone when there is no value: how captures keep findings.
std::string toText(const Finding& finding);

// What the Document processor tells the user about a frame: the banner's hint, the chips, and which overlay
// is the page's outline that the canvas draws as its guide.
struct Guidance {
    doc_vision::Hint frameHint = doc_vision::Hint::NoPage; // this frame's own
    doc_vision::Hint shownHint = doc_vision::Hint::NoPage; // the banner's, steadied by the frames' times
    doc_vision::ChipStates chips{};
    // The index in ProcessorResult::overlays of the page's outline, which the canvas draws as its guide.
    std::optional<std::size_t> outlineOverlay{};
    // Why there is no guidance in this run: the model, or ONNX Runtime, could not be opened. The banner
    // chooses its words by it.
    enum class Unavailable { None, Model, Runtime };
    Unavailable unavailable = Unavailable::None;
    std::string reason{}; // the error, for the status bar
};

// What a processor found in one frame.
struct ProcessorResult {
    std::vector<visual_debug::Overlay> overlays{}; // over the camera frame: on the canvas and sent with it
    std::vector<ProcessedImage> images{};          // debug pictures
    std::vector<Finding> findings{};               // the results panel's rows, such as {"Edges", "4.2 %"}
    std::optional<Guidance> guidance{};            // the Document processor's only
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
