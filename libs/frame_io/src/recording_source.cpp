#include "frame_io/recording.h"

#include "file_sequence.h"
#include "frame_io/image_folder.h"
#include "frame_io/video_source.h"
#include "tags_json.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <system_error>

namespace frame_io {
namespace {

// A recording names its frames by file name alone; anything with a folder or a root in it (a recording sent
// by someone, edited to point elsewhere) would read images from outside the recording.
bool isBareFileName(const std::string& file) {
    const std::filesystem::path path = fromUtf8(file);
    return !file.empty() && path == path.filename() && !path.has_root_path() && path != "." && path != "..";
}

// Frames a second from the frames' own times, first to last; 30, as for a folder, when they do not say.
double rateOf(const std::vector<SequenceEntry>& entries) {
    if (entries.size() < 2 || entries.back().at <= entries.front().at) {
        return 30.0;
    }
    return static_cast<double>(entries.size() - 1) * 1e9 /
           static_cast<double>((entries.back().at - entries.front().at).count());
}

} // namespace

std::unique_ptr<ReplaySource> openRecording(const std::filesystem::path& folder, std::string& error) {
    std::ifstream stream(folder / "recording.json");
    if (!stream) {
        error = utf8Name(folder) + ": no recording.json";
        return nullptr;
    }
    const nlohmann::json json = nlohmann::json::parse(stream, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("format", "") != "document-lab-recording" ||
        !json.contains("frames") || !json["frames"].is_array()) {
        error = utf8Name(folder) + ": recording.json is not a Document Lab recording";
        return nullptr;
    }
    const visual_debug::ImageDescription recordingTags =
        tagsFromJson(json.contains("tags") ? json["tags"] : nlohmann::json::object());

    std::vector<SequenceEntry> entries;
    std::set<std::string> listed;
    for (const nlohmann::json& frame : json["frames"]) {
        if (!frame.is_object() || !frame.contains("file") || !frame["file"].is_string()) {
            continue;
        }
        const std::string file = frame["file"].get<std::string>();
        if (!isBareFileName(file)) {
            error = utf8Name(folder) + ": recording.json names a file outside its folder";
            return nullptr;
        }
        listed.insert(file);
        const std::int64_t at = frame.value("at", std::int64_t{0});
        const visual_debug::ImageDescription tags =
            frame.contains("tags") ? tagsFromJson(frame["tags"]) : recordingTags;
        entries.push_back(
            {.file = folder / fromUtf8(file), .at = std::chrono::nanoseconds(at), .tags = tags});
    }
    // A recording cut short: the frames written after its JSON was, in name order, a thirtieth of a second
    // apart after the last one listed.
    if (!json.value("complete", false)) {
        const std::int64_t base = entries.empty() ? 0 : entries.back().at.count();
        std::int64_t step = entries.empty() ? 0 : 1;
        for (const std::filesystem::path& file : imageFilesIn(folder)) {
            if (listed.count(utf8Name(file)) > 0) {
                continue;
            }
            entries.push_back({.file = file,
                               .at = std::chrono::nanoseconds(base + step * 1'000'000'000 / 30),
                               .tags = recordingTags});
            ++step;
        }
    }
    if (entries.empty()) {
        error = utf8Name(folder) + ": the recording holds no frames";
        return nullptr;
    }
    const double rate = rateOf(entries);
    return std::make_unique<FileSequence>(std::move(entries), utf8Name(folder), ReadOptions{}, rate);
}

std::unique_ptr<ReplaySource> openReplay(const std::filesystem::path& path, std::string& error) {
    std::error_code failed;
    if (std::filesystem::is_directory(path, failed)) {
        if (std::filesystem::exists(path / "recording.json", failed)) {
            return openRecording(path, error);
        }
        return openImageFolder(path, error);
    }
    if (isImageFileName(path)) {
        return openImageFile(path, error);
    }
    return openVideo(path, error);
}

} // namespace frame_io
