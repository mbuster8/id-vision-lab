#pragma once

#include "frame_io/replay_source.h"

#include <filesystem>
#include <memory>
#include <string>

namespace frame_io {

// A video file as a replay source, read ahead three frames on a thread of its own. With FFmpeg's libraries, a
// video more than 8 bits deep or tagged HLG or PQ is read through them, as 16-bit frames with its tags; any
// other through OpenCV's videoio (AVFoundation first on macOS), as 8-bit frames. FFmpeg tells a failed read
// from the end; OpenCV cannot, so its failures end the replay. Restart opens the file again; it cannot seek.
// Null, with `error`, for a file nothing opens.
std::unique_ptr<ReplaySource> openVideo(const std::filesystem::path& file, std::string& error);

} // namespace frame_io
