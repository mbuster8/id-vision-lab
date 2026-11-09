#pragma once

// An image's tags in the lab's JSON files, with the wire format's names ("sdr", "hlg", "pq", "bt709",
// "bt2020"). Private to frame_io, so nlohmann/json stays out of its public headers.

#include "visual_debug/image_description.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace frame_io {

nlohmann::json toJson(const visual_debug::ImageDescription& tags);
visual_debug::ImageDescription tagsFromJson(const nlohmann::json& json);

// Bytes written to `file` whole, then made readable by their owner only (on macOS and Linux). False, with
// `error`, on failure.
bool writeOwnerOnly(const std::filesystem::path& file, const std::vector<unsigned char>& bytes,
                    std::string& error);
bool writeOwnerOnly(const std::filesystem::path& file, const std::string& text, std::string& error);

// Closes `stream`, which wrote `file`, and says whether every byte reached it: a full disk often refuses them
// only as the buffer is flushed on closing. On failure, `error` names the file and the file is removed if
// this write `created` it and it is a regular file (never a device such as /dev/full).
bool finishWrite(std::ofstream& stream, const std::filesystem::path& file, bool created, std::string& error);

// A file's name as UTF-8, whatever the platform's encoding of paths.
std::string utf8Name(const std::filesystem::path& path);
// A path from UTF-8 text (std::filesystem::u8path, deprecated in C++20).
std::filesystem::path fromUtf8(const std::string& text);

} // namespace frame_io
