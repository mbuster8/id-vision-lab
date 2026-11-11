#include "lab_core/processors.h"

#include "lab_core/document_outline.h"
#include "lab_core/document_processor.h"

#include "number_text.h"

#include <doc_vision/page_detector.h>

#include <opencv2/core.hpp>

#include <string>
#include <utility>

namespace lab_core {

namespace {

class NoProcessor final : public FrameProcessor {
public:
    ProcessorResult process(const frame_io::Frame&) override { return {}; }
};

class EdgesProcessor final : public FrameProcessor {
public:
    ProcessorResult process(const frame_io::Frame& frame) override {
        cv::Mat edges = edgesOf(frame.image);
        const double share =
            edges.empty() ? 0.0 : 100.0 * cv::countNonZero(edges) / static_cast<double>(edges.total());
        ProcessorResult result;
        result.findings.push_back({"Edges", oneDecimal(share) + " %"});
        result.images.push_back({.name = "edges", .image = std::move(edges), .description = {}});
        return result;
    }
};

class OutlineProcessor final : public FrameProcessor {
public:
    ProcessorResult process(const frame_io::Frame& frame) override {
        ProcessorResult result;
        if (std::optional<std::vector<cv::Point2d>> corners = m_hold.next(documentOutline(frame.image))) {
            result.overlays.push_back(visual_debug::Polygon{std::move(*corners), "Document"});
            result.findings.push_back({"Document", "found"});
        } else {
            result.findings.push_back({"Document", "not found"});
        }
        return result;
    }

    void reset() override { m_hold = {}; }

private:
    OutlineHold m_hold;
};

} // namespace

std::string toText(const Finding& finding) {
    return finding.value.empty() ? finding.name : finding.name + ": " + finding.value;
}

std::string_view nameOf(ProcessorKind kind) {
    switch (kind) {
    case ProcessorKind::None:
        return "None";
    case ProcessorKind::Edges:
        return "Edges";
    case ProcessorKind::Outline:
        return "Outline";
    case ProcessorKind::Document:
        return "Document";
    }
    return "None";
}

std::unique_ptr<FrameProcessor> makeProcessor(ProcessorKind kind, const ProcessorSettings& settings) {
    switch (kind) {
    case ProcessorKind::Edges:
        return std::make_unique<EdgesProcessor>();
    case ProcessorKind::Outline:
        return std::make_unique<OutlineProcessor>();
    case ProcessorKind::Document:
        return std::make_unique<DocumentProcessor>(
            [model = settings.modelFolder / kLc100ModelFile](
                std::string& error,
                doc_vision::OpenFailure& failure) -> std::unique_ptr<doc_vision::PageFinder> {
                return doc_vision::PageDetector::open(model, error, &failure);
            });
    case ProcessorKind::None:
        break;
    }
    return std::make_unique<NoProcessor>();
}

} // namespace lab_core
