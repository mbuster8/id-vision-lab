#pragma once

#include <doc_vision/guidance.h>
#include <doc_vision/mrz.h>
#include <frame_io/frame.h>
#include <visual_debug/image_description.h>
#include <visual_debug/overlay.h>

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
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

// The page the Document processor found in this frame, for the focus controller (A3.3): never one PageHold
// kept over a missed detection.
struct PageFacts {
    doc_vision::Quad quad{}; // aligned to the previous quad, as the outline is
    cv::Size frameSize{};
    double sharpness = 0.0;  // doc_vision::Measures::sharpness
    double longerSide = 0.0; // doc_vision::Measures::longerSide
    doc_vision::Verdicts verdicts{};
    doc_vision::Hint frameHint = doc_vision::Hint::NoPage; // this frame's own
};

// The MRZ reader's state while the Document processor reads (A4, spec §4.2).
struct MrzProgress {
    bool available = true;                // false: the reader could not be opened
    std::string reason;                   // why, in fixed words, when unavailable
    int agreeing = 0;                     // the most identical valid reads in the window, at most 3
    std::optional<doc_vision::Quad> band; // in this frame, when it is drawn
};

// An accepted MRZ (A4): the frame kept for the user to review, save or send.
struct MrzScan {
    frame_io::Frame frame;      // the sharpest of the agreeing reads' frames
    doc_vision::Quad quad{};    // the document's, the right way up: corner 1 is the page's top left as read
    doc_vision::Quad band{};    // the MRZ band's, in the frame, the right way up
    std::vector<Finding> gates; // that frame's Fill, Tilt, Sharpness, Glare and Exposure rows
    doc_vision::Mrz mrz;
    std::chrono::nanoseconds timeToScan{}; // from the first fresh page after the window was last cleared
    int reads = 0;                         // reads made since then
};

// A4.1: a TD1 card's front, kept after its back's MRZ was accepted and the card turned over (spec §6.4).
struct FrontShot {
    frame_io::Frame frame;
    doc_vision::Quad quad{};    // the outline as found and aligned: corner 1 as the outline numbers it
    std::vector<Finding> gates; // that frame's Fill, Tilt, Sharpness, Glare and Exposure rows
};

// Where the Document processor's front stage is: waiting for the card to be turned (its page lost for longer
// than PageHold bridges, or the frames before forgotten by a reset), then looking for a front to keep.
enum class FrontStage { AwaitingTurn, Looking };

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
    std::string reason{};            // the error, for the status bar
    std::optional<PageFacts> page{}; // the page found in this frame, if any
    // A4: while the processor has a reader and has accepted nothing.
    std::optional<MrzProgress> reading{};
    // The index in ProcessorResult::overlays of the MRZ band, drawn as a dashed guide.
    std::optional<std::size_t> bandOverlay{};
    // The accepted MRZ, on every result from the accept until reset(): the engine keeps only the newest
    // result, so one result alone could be missed.
    std::shared_ptr<const MrzScan> scan{};
    // A4.1: while the front stage is on, where it is; nothing is read meanwhile.
    std::optional<FrontStage> frontStage{};
    // The front kept, on every result from its keeping until reset() or the stage's end, as `scan` is.
    std::shared_ptr<const FrontShot> front{};
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
    // A4.1: look for a TD1 card's front after its back was accepted (on), or stop (off). Only the Document
    // processor has a front stage.
    virtual void setFrontStage(bool on) { static_cast<void>(on); }
};

} // namespace lab_core
