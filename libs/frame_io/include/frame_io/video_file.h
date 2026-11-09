#pragma once

#include <opencv2/videoio.hpp>

#include <string>

namespace frame_io {

// Opens a video file for reading. On macOS it tries OpenCV's AVFoundation backend first, which decodes on the
// Mac's media engine and keeps the colours of HDR (HLG) video close to QuickTime's: an iPhone's 4K HEVC
// 10-bit 60 fps file reads at about 260 frames a second there against about 48 through FFmpeg, measured on
// an M4. A file AVFoundation cannot open (Matroska, for one), or opens and cannot read a frame of, opens with
// OpenCV's default backend. False when nothing opens it.
bool openVideoFile(cv::VideoCapture& capture, const std::string& path);

} // namespace frame_io
