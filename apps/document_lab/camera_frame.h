#pragma once

#include <frame_io/frame.h>

#include <QVideoFrame>

#include <chrono>
#include <optional>
#include <string>

namespace document_lab {

// What a camera's frames need beyond their own format.
struct CameraGeometry {
    bool frontCamera = false; // darwin mirrors a front camera's pixels without flagging it
};

// The frame upright and unmirrored as BGR (8-bit; 16-bit for P010, with its tags), at `at`. Converted by the
// frame's own pixel format, never the chosen camera format's (a camera may send another), with the matrix and
// the range its format names. Nothing, with `error`, for a format it cannot convert or a frame it cannot map.
// Maps a copy of the frame, which shares its buffer, so `frame` itself is left as it was.
std::optional<frame_io::Frame> toFrame(const QVideoFrame& frame, CameraGeometry geometry,
                                       std::chrono::nanoseconds at, std::string& error);

// Clockwise degrees, 0, 90, 180 or 270: the frame's own rotation, plus its format's from Qt 6.8.
int rotationOf(const QVideoFrame& frame);

} // namespace document_lab
