#pragma once

#include "image_view.h"
#include "live_converter.h"

#include <opencv2/core/types.hpp>

#include <optional>

// The rules a window follows to draw a live frame through a LiveConverter, shared by the viewer and the
// Document Lab so both convert the same part of a frame at the same size.

// The size a part of a frame is converted at when drawn at `scale` device pixels per frame pixel: the drawn
// size when that is smaller than the part, else the part's own.
cv::Size drawnSizeOf(cv::Size part, double scale);

// What to convert of a frame this size for a draw with `keep`: zoomed in, the part of the frame in view and a
// quarter of its width and height on each side, so a small pan shows converted pixels at once; the whole
// frame when that part would be most of it anyway, where cutting saves little and every pan would cost a
// conversion.
LiveDrawing liveDrawing(const ImageView& view, cv::Size frameSize, ImageView::KeepView keep);

// Whether a zoom, fit, resize or pan needs the frame converted again: unless the newest job's picture still
// covers what is in view at the size it would be converted at now. Past 1:1 its size stays the area's at
// every step, and converting again for each step of a trackpad zoom, or each move of a pan within the margin,
// would only cost. Compared with the newest job, not the picture on screen, which a job still converting is
// about to replace.
bool needsRedraw(const ImageView& view, cv::Size frameSize, const std::optional<LiveDrawing>& newest);
