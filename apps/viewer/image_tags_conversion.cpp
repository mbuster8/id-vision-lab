#include "image_tags_conversion.h"

namespace {

// Both enums list the same values in the same order; the casts rely on it, and these checks keep it so.
static_assert(static_cast<int>(viewer_core::Transfer::Pq) == static_cast<int>(visual_debug::Transfer::Pq));
static_assert(static_cast<int>(viewer_core::Primaries::Bt2020) ==
              static_cast<int>(visual_debug::Primaries::Bt2020));

template <typename To, typename From> std::optional<To> converted(const std::optional<From>& value) {
    return value ? std::optional<To>(static_cast<To>(*value)) : std::nullopt;
}

} // namespace

viewer_core::ImageTags toImageTags(const visual_debug::ImageDescription& description) {
    return {.valueRange = {description.valueRange.min, description.valueRange.max},
            .transfer = converted<viewer_core::Transfer>(description.transfer),
            .primaries = converted<viewer_core::Primaries>(description.primaries)};
}

visual_debug::ImageDescription toDescription(const viewer_core::ImageTags& tags) {
    return {.valueRange = {tags.valueRange.min, tags.valueRange.max},
            .transfer = converted<visual_debug::Transfer>(tags.transfer),
            .primaries = converted<visual_debug::Primaries>(tags.primaries)};
}
