#include "lab_core/processors.h"

#include "lab_core/document_outline.h"

#include <opencv2/core.hpp>

#include <array>
#include <charconv>
#include <string>
#include <utility>

namespace lab_core {

namespace {

// std::to_chars never consults the locale, so the finding reads "4.2 %" whatever the user's locale.
std::string oneDecimal(double value) {
    std::array<char, 32> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 1);
    return {buffer.data(), result.ptr};
}

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
    }
    return "None";
}

std::unique_ptr<FrameProcessor> makeProcessor(ProcessorKind kind) {
    switch (kind) {
    case ProcessorKind::Edges:
        return std::make_unique<EdgesProcessor>();
    case ProcessorKind::Outline:
        return std::make_unique<OutlineProcessor>();
    case ProcessorKind::None:
        break;
    }
    return std::make_unique<NoProcessor>();
}

} // namespace lab_core
