#pragma once

#include "viewer_core/display_settings.h"
#include "viewer_core/image_tags.h"

#include <opencv2/core/mat.hpp>

namespace viewer_core {

// The range an HDR signal runs over, black to nominal white: the tags' value-range hint, else the depth's
// full range (0-255 for 8U, 0-65535 for 16U, the type's range for other integers, 0-1 for floating point).
DisplayRange hdrSignalRange(int depth, const ImageTags& tags);

// The top of the scale an HDR signal is written on: the depth's maximum for integers, the signal range's
// maximum for floating point. For HLG, the codes between nominal white and it are super-whites.
double hdrScaleTop(int depth, const DisplayRange& signalRange);

// Maps an HLG or PQ signal to what an SDR screen shows (ITU-R BT.2100, BT.2408, BT.2087): to display light
// (HLG with its 1000 cd/m² nominal peak, PQ assuming a 1000 cd/m² peak), HDR reference white (203 cd/m²) just
// below the screen's white, brighter light compressed smoothly into the room above it so that the peak is
// white, BT.2020 colours to BT.709, clipped, then the sRGB curve. For HLG the peak is the light of the
// scale's top, so super-whites keep their gradations; PQ is clipped at its nominal white.
//
// `signal` has 1 or 3 channels (BGR), of depth 8U to 64F with no NaN or infinity, already at the size it will
// be drawn. Returns 8-bit BGR (CV_8UC3), or CV_8UC1 for one channel. Throws std::invalid_argument for other
// channel counts, for Transfer::Sdr, and for a signal range whose minimum is not below its maximum.
cv::Mat toneMapToSdr(const cv::Mat& signal, Transfer transfer, Primaries primaries, DisplayRange signalRange);

} // namespace viewer_core
