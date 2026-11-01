#pragma once

#include "viewer_core/display_settings.h"

#include <optional>

namespace viewer_core {

// The same as visual_debug's: viewer_core does not depend on the transport library, so the viewer converts.
enum class Transfer { Sdr, Hlg, Pq };
enum class Primaries { Bt709, Bt2020 };

// What the sender said about an image beyond its pixels; every field is optional.
struct ImageTags {
    DisplayRange valueRange{}; // the values black and white nominally sit at; none unless min < max
    std::optional<Transfer> transfer{};
    std::optional<Primaries> primaries{};

    bool operator==(const ImageTags&) const = default;

    bool hasValueRange() const;
    bool isHdr() const; // HLG or PQ
};

// The settings an image is shown with until the user picks others.
DisplaySettings defaultSettings(const ImageTags& tags);

} // namespace viewer_core
