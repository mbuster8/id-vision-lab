#include "viewer_core/image_tags.h"

#include <cmath>

namespace viewer_core {

bool ImageTags::hasValueRange() const {
    return std::isfinite(valueRange.min) && std::isfinite(valueRange.max) && valueRange.min < valueRange.max;
}

bool ImageTags::isHdr() const { return transfer == Transfer::Hlg || transfer == Transfer::Pq; }

// Auto for every image: with a value-range hint, Auto shows the hint.
DisplaySettings defaultSettings(const ImageTags& /*tags*/) { return DisplaySettings{}; }

} // namespace viewer_core
