#include "viewer_core/image_tags.h"

#include <cmath>

namespace viewer_core {

bool ImageTags::hasValueRange() const {
    return std::isfinite(valueRange.min) && std::isfinite(valueRange.max) && valueRange.min < valueRange.max;
}

bool ImageTags::isHdr() const { return transfer == Transfer::Hlg || transfer == Transfer::Pq; }

// HDR for an HLG or PQ image, which shown raw looks flat or dark; Auto for any other, which with a
// value-range hint shows the hint.
DisplaySettings defaultSettings(const ImageTags& tags) {
    return tags.isHdr() ? DisplaySettings{.mode = RangeMode::Hdr} : DisplaySettings{};
}

} // namespace viewer_core
