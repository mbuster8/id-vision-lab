#pragma once

#include "visual_debug/overlay.h"

#include <optional>
#include <vector>

namespace visual_debug {

// How an image's values map to light, for video that is not plain SDR: hybrid log-gamma (ARIB STD-B67) or
// the perceptual quantiser (SMPTE ST 2084).
enum class Transfer { Sdr, Hlg, Pq };
enum class Primaries { Bt709, Bt2020 };

// The range an image's values nominally span, black to white; values may go beyond it (a video's super-whites
// do). Empty, and then not sent, unless both ends are finite and min < max.
struct ValueRange {
    double min = 0.0;
    double max = 0.0;

    bool empty() const;
    bool operator==(const ValueRange&) const = default;
};

// What a sender may say about an image beyond its pixels. Every field is optional, and a viewer that does not
// know a field ignores it.
struct ImageDescription {
    ValueRange valueRange{}; // a plain struct, not std::optional, so `{.valueRange = {0, 1023}}` compiles
    std::optional<Transfer> transfer{};
    std::optional<Primaries> primaries{};
    // Drawn over the image, in list order, later ones on top; see overlay.h.
    std::vector<Overlay> overlays{};

    bool operator==(const ImageDescription&) const = default;
};

} // namespace visual_debug
