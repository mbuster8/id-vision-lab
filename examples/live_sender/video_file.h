#pragma once

#include <opencv2/videoio.hpp>

#include <string>

namespace live_sender {

// Opens a video file for reading. On macOS it tries OpenCV's AVFoundation backend first, which decodes on the
// Mac's media engine and keeps the colours of HDR (HLG) video close to QuickTime's: an iPhone's 4K HEVC
// 10-bit file reads at 237 frames a second there against 26 through FFmpeg, measured on an M4. A file
// AVFoundation cannot read (Matroska, for one) opens with OpenCV's default backend. False when nothing
// opens it.
bool openVideoFile(cv::VideoCapture& capture, const std::string& path);

} // namespace live_sender
