#pragma once

#include "camera_backend.h"

#include <optional>
#include <vector>

namespace document_lab {

// One of each (size, maximum rate, pixel format), most pixels first, then the highest rate, then by format.
std::vector<CameraFormat> distinctFormats(std::vector<CameraFormat> formats);

// The format a camera starts with until the user picks another: uncompressed (JPEG costs a decode per frame)
// before compressed, then the most pixels, then the highest rate up to 60 fps (a faster one only when nothing
// is at or under 60), then NV12. Nothing for no formats.
std::optional<CameraFormat> defaultFormat(const std::vector<CameraFormat>& formats);

} // namespace document_lab
