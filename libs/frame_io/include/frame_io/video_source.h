#pragma once

#include "frame_io/replay_source.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace frame_io {

struct VideoOptions {
    // Called on the reading thread just before each seek, with whether that seek has been given up (a newer
    // seek, a restart or the source's end); tests hold a seek here.
    std::function<void(const std::function<bool()>& abandoned)> beforeSeek{};
};

// A video file as a replay source, read ahead three frames on a thread of its own. With FFmpeg's libraries
// every video is read through them: an 8-bit SDR one as 8-bit frames, anything deeper or HLG or PQ as 16-bit
// frames with its tags; a video whose frames have timestamps can seek, to the exact frame, on the reading
// thread. Without them, or for a file they do not open, through OpenCV's videoio (AVFoundation first on
// macOS), as 8-bit frames that cannot seek. FFmpeg tells a failed read from the end; OpenCV cannot, so its
// failures end the replay. Restart opens the file again. Null, with `error`, for a file nothing opens.
std::unique_ptr<ReplaySource> openVideo(const std::filesystem::path& file, std::string& error,
                                        VideoOptions options = {});

} // namespace frame_io
