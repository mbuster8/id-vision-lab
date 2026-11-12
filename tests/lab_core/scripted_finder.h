#pragma once

#include "procedural_page.h"

#include "lab_core/document_processor.h"

#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>
#include <frame_io/frame.h>

#include <opencv2/core.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A page finder the tests script, and the frames the Document processor's tests feed it: shared by the
// processor's, the engine's and the privacy tests.
namespace test_support {

// One answer of the scripted finder: a page, nothing, or nothing with an error.
struct Step {
    std::optional<doc_vision::Quad> quad;
    std::string error;
};

inline Step page(const doc_vision::Quad& quad) { return {.quad = quad, .error = {}}; }
inline Step nothing() { return {}; }
inline Step failure(std::string error) { return {.quad = std::nullopt, .error = std::move(error)}; }

// What the scripted finders of one processor share with the test: the answers in order (the last one
// repeats), the frames they were given, and how often the processor asked for a finder.
struct Script {
    std::vector<Step> steps;
    std::size_t calls = 0;
    int opens = 0;
    cv::Mat lastImage;
};

class ScriptedFinder final : public doc_vision::PageFinder {
public:
    explicit ScriptedFinder(std::shared_ptr<Script> script) : m_script(std::move(script)) {}

    std::optional<doc_vision::PageFound> find(const cv::Mat& bgr8, std::string& error) override {
        Script& script = *m_script;
        script.lastImage = bgr8.clone();
        if (script.steps.empty()) {
            return std::nullopt;
        }
        const Step& step = script.steps[std::min(script.calls++, script.steps.size() - 1)];
        error = step.error;
        if (!step.quad) {
            return std::nullopt;
        }
        return doc_vision::PageFound{.quad = *step.quad, .weakestPeak = 1.0};
    }

private:
    std::shared_ptr<Script> m_script;
};

inline lab_core::DocumentProcessor::MakeFinder scripted(const std::shared_ptr<Script>& script) {
    return [script](std::string&, doc_vision::OpenFailure&) -> std::unique_ptr<doc_vision::PageFinder> {
        ++script->opens;
        return std::make_unique<ScriptedFinder>(script);
    };
}

inline const cv::Size kFrameSize{1280, 720};

// The frame TheProceduralPagePassesEveryGate judges: every gate passes.
inline cv::Mat goodFrame() { return pageInFrame(proceduralPage(), kPageQuad, kFrameSize); }

// Frame `n`, 33 ms after the one before.
inline frame_io::Frame frameAt(const cv::Mat& image, int n, visual_debug::ImageDescription tags = {}) {
    return {.image = image,
            .number = static_cast<std::uint64_t>(n),
            .at = std::chrono::milliseconds{33 * n},
            .tags = std::move(tags)};
}

} // namespace test_support
