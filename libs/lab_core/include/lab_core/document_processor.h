#pragma once

#include "lab_core/frame_processor.h"

#include <doc_vision/guidance.h>
#include <doc_vision/onnx_model.h>
#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>
#include <doc_vision/quality_gates.h>

#include <opencv2/core/mat.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace lab_core {

// The Document processor: a PageFinder finds the page, the six gates judge it, and the hint, the chips and a
// briefly lost page are steadied by the frames' own times (ADR-004). Its results carry Guidance, the page's
// outline as an overlay, the findings, and the 625 × 440 page the gates looked at as the picture "page".
class DocumentProcessor final : public FrameProcessor {
public:
    // Opens the finder: on the processing thread, on a run's first frame. Null, with `error` and `failure`,
    // when the model or ONNX Runtime cannot be opened.
    using MakeFinder = std::function<std::unique_ptr<doc_vision::PageFinder>(
        std::string& error, doc_vision::OpenFailure& failure)>;

    // `thresholds`: the gates' bands, the defaults unless an evaluation tries others.
    explicit DocumentProcessor(MakeFinder makeFinder, doc_vision::Thresholds thresholds = {});

    ProcessorResult process(const frame_io::Frame& frame) override;
    // Clears the timers, the page hold, the GateJudge, the previous quad, and a failed open (the next frame
    // tries again). It keeps a finder that opened: the engine resets on every seek, loop and restart.
    void reset() override;

private:
    MakeFinder m_makeFinder;
    std::unique_ptr<doc_vision::PageFinder> m_finder;
    Guidance::Unavailable m_unavailable = Guidance::Unavailable::None; // the open failed: not tried again
    std::string m_reason;

    doc_vision::GateJudge m_judge;
    doc_vision::HintTimer m_hintTimer;
    doc_vision::ChipTimer m_chipTimer;
    doc_vision::PageHold m_hold;
    std::optional<doc_vision::Quad> m_previous; // the page's last quad, so its start corner does not flip
    doc_vision::Measures m_measures{};          // of the page last found: shown while it is held
    cv::Mat m_page;                             // its gate page
};

} // namespace lab_core
