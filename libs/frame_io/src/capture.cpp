#include "frame_io/capture.h"

#include "tags_json.h"

#include <visual_debug/wire_format.h>

#include <opencv2/imgcodecs.hpp>

#include <nlohmann/json.hpp>

#include <system_error>

namespace frame_io {

std::filesystem::path writeCapture(const std::filesystem::path& folder, const std::string& baseName,
                                   const Frame& frame, const CaptureInfo& info, std::string& error) {
    for (const CaptureSection& section : info.sections) {
        if (section.name == "source" || section.name == "frame" || section.name == "at" ||
            section.name == "tags" || section.name == "findings" || section.name == "overlays") {
            error = "a capture's section may not be named like one of its own keys";
            return {};
        }
    }
    const std::string stem = baseName + " frame " + std::to_string(frame.number + 1);
    std::filesystem::path png;
    std::filesystem::path json;
    for (int attempt = 1; attempt < 1000; ++attempt) {
        const std::string name = attempt == 1 ? stem : stem + " " + std::to_string(attempt);
        png = folder / fromUtf8(name + ".png");
        json = folder / fromUtf8(name + ".json");
        std::error_code ignored;
        if (!std::filesystem::exists(png, ignored) && !std::filesystem::exists(json, ignored)) {
            break;
        }
    }
    std::vector<unsigned char> bytes;
    try {
        if (!cv::imencode(".png", frame.image, bytes)) {
            error = "the frame could not be encoded as PNG";
            return {};
        }
    } catch (const cv::Exception&) {
        error = "the frame could not be encoded as PNG";
        return {};
    }
    // Ordered, so a section's values keep the order the caller gave (a plain json object sorts its keys; one
    // nested in it would too).
    nlohmann::ordered_json described{
        {"source", info.source},
        {"frame", frame.number + 1},
        {"at", frame.at.count()},
        {"tags", nlohmann::ordered_json::parse(
                     toJson(frame.tags).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))},
        {"findings", info.findings}};
    // In the wire format's shape, as the viewer would have drawn them.
    if (!info.overlays.empty()) {
        described["overlays"] = nlohmann::ordered_json::parse(visual_debug::encodeOverlays(info.overlays));
    }
    for (const CaptureSection& section : info.sections) {
        nlohmann::ordered_json values = nlohmann::ordered_json::object();
        for (const auto& [name, value] : section.values) {
            values[name] = value;
        }
        described[section.name] = std::move(values);
    }
    // A finding or a value that is not UTF-8 is written with U+FFFD in its place rather than thrown on.
    const std::string text =
        described.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    if (!writeOwnerOnly(png, bytes, error) || !writeOwnerOnly(json, text, error)) {
        return {};
    }
    return png;
}

} // namespace frame_io
