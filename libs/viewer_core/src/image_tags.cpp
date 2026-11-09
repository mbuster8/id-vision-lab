#include "viewer_core/image_tags.h"

#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>

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

ImageTags tagsForCrop(const ImageTags& tags, cv::Point origin) {
    ImageTags cropped = tags;
    if (tags.overlayList().empty()) {
        return cropped;
    }
    const cv::Point2d by(origin.x, origin.y);
    std::vector<Overlay> moved = tags.overlayList(); // a copy: the frame's own list is shared, never changed
    for (Overlay& overlay : moved) {
        std::visit(
            [&by](auto& shape) {
                using Shape = std::decay_t<decltype(shape)>;
                if constexpr (std::is_same_v<Shape, Polygon>) {
                    for (cv::Point2d& point : shape.points) {
                        point -= by;
                    }
                } else if constexpr (std::is_same_v<Shape, Rectangle>) {
                    shape.rect.x -= by.x;
                    shape.rect.y -= by.y;
                } else if constexpr (std::is_same_v<Shape, Circle>) {
                    shape.centre -= by;
                } else {
                    shape.at -= by;
                }
            },
            overlay);
    }
    cropped.overlays = makeOverlays(std::move(moved));
    return cropped;
}

// HDR for an HLG or PQ image, which shown raw looks flat or dark; Auto for any other, which with a
// value-range hint shows the hint.
DisplaySettings defaultSettings(const ImageTags& tags) {
    return tags.isHdr() ? DisplaySettings{.mode = RangeMode::Hdr} : DisplaySettings{};
}

} // namespace viewer_core
