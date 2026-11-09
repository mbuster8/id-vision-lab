#pragma once

#include "frame_io/replay_source.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frame_io {

// File names compared with runs of digits as numbers: "frame2" before "frame10". The same number written with
// more leading zeros comes after, so the order stays total.
bool naturalLess(std::string_view a, std::string_view b);

// Whether a file's extension is one of the image formats OpenCV reads (PNG, JPEG, TIFF, BMP, WebP and the
// like). By name only: a file with the name of an image that does not decode is an error when it is read.
bool isImageFileName(const std::filesystem::path& file);

// The image files in a folder (not its subfolders), in natural order of their names.
std::vector<std::filesystem::path> imageFilesIn(const std::filesystem::path& folder);

// How a folder's frames are read ahead of the one asked for.
struct ReadOptions {
    std::size_t depth = 4;                               // frames decoded ahead, in order
    std::function<void(std::uint64_t index)> beforeRead; // tests hold a reading thread here
};

// The frames a folder of images is played as: in natural order, as if 30 a second apart (frame n at
// n × 1e9 / 30 ns), read ahead on threads of their own. Null, with `error`, for a folder without images.
std::unique_ptr<ReplaySource> openImageFolder(const std::filesystem::path& folder, std::string& error,
                                              ReadOptions options = {});
// One image as a source of one frame. Null, with `error`, for a file that does not decode or is not 8- or
// 16-bit with 1, 3 or 4 channels.
std::unique_ptr<ReplaySource> openImageFile(const std::filesystem::path& file, std::string& error);

// Reads and decodes one image, keeping its depth and channels. Through std::filesystem, so any path works on
// Windows too. Empty, with `error`, when it cannot be read, does not decode or is not accepted.
cv::Mat readImage(const std::filesystem::path& file, std::string& error);

} // namespace frame_io
