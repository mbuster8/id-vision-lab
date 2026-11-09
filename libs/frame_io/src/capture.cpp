#include "frame_io/capture.h"

#include "tags_json.h"

#include <opencv2/imgcodecs.hpp>

#include <nlohmann/json.hpp>

#include <system_error>

namespace frame_io {

std::filesystem::path writeCapture(const std::filesystem::path& folder, const std::string& baseName,
                                   const Frame& frame, const CaptureInfo& info, std::string& error) {
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
    const nlohmann::json described{{"source", info.source},
                                   {"frame", frame.number + 1},
                                   {"at", frame.at.count()},
                                   {"tags", toJson(frame.tags)},
                                   {"findings", info.findings}};
    if (!writeOwnerOnly(png, bytes, error) || !writeOwnerOnly(json, described.dump(2) + "\n", error)) {
        return {};
    }
    return png;
}

} // namespace frame_io
