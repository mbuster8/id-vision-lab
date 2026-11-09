#pragma once

#include "frame_io/frame.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace frame_io {

enum class ReadStatus { Frame, End, Error };

struct ReadResult {
    ReadStatus status = ReadStatus::End;
    Frame frame;       // with ReadStatus::Frame
    std::string error; // with ReadStatus::Error: why, naming the file
};

// Frames read from a file or a folder, in order, for a replay. Used from one thread at a time; the wake is
// called from the source's own reading threads.
class ReplaySource {
public:
    virtual ~ReplaySource() = default;

    // The next frame in order, its end, or an error, which is never the end. May wait until ready().
    virtual ReadResult next() = 0;
    // Whether next() would return at once.
    virtual bool ready() const = 0;
    // Called, from the source's reading thread, whenever ready() may have turned true; so a caller can wait
    // on its own condition for a frame and for anything else at once.
    virtual void setWake(std::function<void()> wake) = 0;
    // Back to frame 0: by seeking, or by opening the file again. False, with `error`, when it cannot.
    virtual bool restart(std::string& error) = 0;
    // Folders and recordings can go to any frame; a video file only back to its start.
    virtual bool canSeek() const = 0;
    // Only when canSeek(): next() then returns frame `index` (numbered `index`), or the end past the last.
    virtual void seek(std::uint64_t index) = 0;
    virtual std::optional<std::uint64_t> frameCount() const = 0;
    // The file's or the folder's name, for the window and recording.json.
    virtual std::string describe() const = 0;
};

} // namespace frame_io
