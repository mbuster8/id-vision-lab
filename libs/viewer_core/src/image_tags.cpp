#include "viewer_core/image_tags.h"

#include <cmath>

namespace viewer_core {

bool ImageTags::hasValueRange() const {
    return std::isfinite(valueRange.min) && std::isfinite(valueRange.max) && valueRange.min < valueRange.max;
}

bool ImageTags::isHdr() const { return transfer == Transfer::Hlg || transfer == Transfer::Pq; }

const std::vector<Overlay>& ImageTags::overlayList() const {
    static const std::vector<Overlay> kNone;
    return overlays ? *overlays : kNone;
}

bool ImageTags::operator==(const ImageTags& other) const {
    return valueRange == other.valueRange && transfer == other.transfer && primaries == other.primaries &&
           (overlays == other.overlays || overlayList() == other.overlayList());
}

ImageTags::Overlays makeOverlays(std::vector<Overlay> overlays) {
    if (overlays.empty()) {
        return nullptr;
    }
    return std::make_shared<const std::vector<Overlay>>(std::move(overlays));
}

// HDR for an HLG or PQ image, which shown raw looks flat or dark; Auto for any other, which with a
// value-range hint shows the hint.
DisplaySettings defaultSettings(const ImageTags& tags) {
    return tags.isHdr() ? DisplaySettings{.mode = RangeMode::Hdr} : DisplaySettings{};
}

} // namespace viewer_core
