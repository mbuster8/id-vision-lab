#pragma once

#include "frame_io/frame.h"

#include <filesystem>
#include <string>
#include <vector>

namespace frame_io {

// What is known about a captured frame besides its pixels.
struct CaptureInfo {
    std::string source;                            // the camera's or the file's name
    std::vector<std::string> findings;             // the processor's, for this frame
    std::vector<visual_debug::Overlay> overlays{}; // the processor's: where it found what it found
};

// Keeps a frame at full resolution: "<baseName> frame <number + 1>.png" (8- or 16-bit as the frame is) and a
// .json beside it with the source, the frame's number as the user sees it, its time, its tags and the
// findings; " 2", " 3"… are added when the name is taken. Readable by their owner only on macOS and Linux.
// The PNG's path, or empty with `error`.
std::filesystem::path writeCapture(const std::filesystem::path& folder, const std::string& baseName,
                                   const Frame& frame, const CaptureInfo& info, std::string& error);

} // namespace frame_io
