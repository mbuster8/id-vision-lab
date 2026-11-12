#pragma once

#include "lab_core/frame_processor.h"
#include "lab_core/mrz_worker.h"

#include <doc_vision/guidance.h>
#include <doc_vision/mrz_agreement.h>
#include <doc_vision/onnx_model.h>
#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>
#include <doc_vision/quality_gates.h>

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lab_core {

// The Document processor's MRZ reader (A4), made on the worker's thread on the first read; no `make`, no
// reading. At namespace scope: a default argument of the class it is nested in could not use its
// initialisers.
struct ReaderSetup {
    MrzWorker::MakeReader make{};
    MrzWorker::Mode mode = MrzWorker::Mode::Threaded;
};

// The Document processor: a PageFinder finds the page, the six gates judge it, and the hint, the chips and a
// briefly lost page are steadied by the frames' own times (ADR-004). Its results carry Guidance, the page's
// outline as an overlay, the findings, and the 625 × 440 page the gates looked at as the picture "document".
// With a reader (A4), it reads the MRZ of every frame whose page passes every gate on a worker thread, and
// accepts it when three whole reads of three frames agree. A read is collected at the start of the next
// process(), in every worker mode.
class DocumentProcessor final : public FrameProcessor {
public:
    // Opens the finder: on the processing thread, on a run's first frame. Null, with `error` and `failure`,
    // when the model or ONNX Runtime cannot be opened.
    using MakeFinder = std::function<std::unique_ptr<doc_vision::PageFinder>(
        std::string& error, doc_vision::OpenFailure& failure)>;
    using Reader = ReaderSetup;

    // `thresholds`: the gates' bands, the defaults unless an evaluation tries others.
    explicit DocumentProcessor(MakeFinder makeFinder, doc_vision::Thresholds thresholds = {},
                               Reader reader = {});

    ProcessorResult process(const frame_io::Frame& frame) override;
    // Clears the timers, the page hold, the GateJudge, the previous quad, and a failed open (the next frame
    // tries again). It keeps a finder that opened: the engine resets on every seek, loop and restart. It also
    // forgets the reads, the agreement and an accepted scan, and reads still coming are dropped; the reader
    // stays open.
    void reset() override;

    // Manual mode: reads the job waiting. Whether there was one.
    bool pumpReaderForTests();

private:
    // A valid passport read's frame, kept while its read is in the agreement's window: only the sharpest per
    // MRZ text.
    struct Kept {
        std::string text;
        MrzFrameRecord record;
        doc_vision::Quad bandOnPage{}; // as read
        bool turned = false;
    };

    void collectReads();
    void accept(const doc_vision::Mrz& mrz, const MrzDone& accepting);
    std::vector<Finding> readerFindings() const;
    std::vector<Finding> scanFindings() const;

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

    // A4's reading.
    std::unique_ptr<MrzWorker> m_worker; // null without a reader
    std::uint64_t m_resetCount = 0;      // reads of an older count are dropped
    doc_vision::MrzAgreement m_agreement;
    std::vector<Kept> m_kept;
    int m_reads = 0;                                        // since the window was last cleared
    bool m_turned = false;                                  // the next job's page is read turned first
    std::optional<std::chrono::nanoseconds> m_firstPageAt;  // the window's first fresh page
    std::optional<std::chrono::nanoseconds> m_lastReadTime; // the last read's two stages
    std::string m_lastReadError;
    std::shared_ptr<const MrzScan> m_scan;
};

} // namespace lab_core
