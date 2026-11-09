#pragma once

#include "viewer_core/display_settings.h"
#include "viewer_core/overlay.h"

#include <memory>
#include <optional>
#include <vector>

namespace viewer_core {

// The same as visual_debug's: viewer_core does not depend on the transport library, so the viewer converts.
enum class Transfer { Sdr, Hlg, Pq };
enum class Primaries { Bt709, Bt2020 };

// What the sender said about an image beyond its pixels; every field is optional.
struct ImageTags {
    // Shared and never changed once made: tags are copied for every live frame, draw and history row, and a
    // list may hold up to 64 KiB of shapes. Null for none.
    using Overlays = std::shared_ptr<const std::vector<Overlay>>;

    DisplayRange valueRange{}; // the values black and white nominally sit at; none unless min < max
    std::optional<Transfer> transfer{};
    std::optional<Primaries> primaries{};
    Overlays overlays{};

    // Overlays compare by content, a null list equal to an empty one.
    bool operator==(const ImageTags& other) const;

    bool hasValueRange() const;
    bool isHdr() const;                              // HLG or PQ
    const std::vector<Overlay>& overlayList() const; // empty for none
};

// The list as ImageTags holds it: null for an empty one.
ImageTags::Overlays makeOverlays(std::vector<Overlay> overlays);

// The tags of a part of an image whose top-left corner is at `origin`: the shapes moved by -origin, so they
// stay over the same pixels; everything else as it was.
ImageTags tagsForCrop(const ImageTags& tags, cv::Point origin);

// The settings an image is shown with until the user picks others.
DisplaySettings defaultSettings(const ImageTags& tags);

} // namespace viewer_core
