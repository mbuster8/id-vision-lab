#include "image_tags_conversion.h"

#include <variant>
#include <vector>

namespace {

// Both enums list the same values in the same order; the casts rely on it, and these checks keep it so.
static_assert(static_cast<int>(viewer_core::Transfer::Pq) == static_cast<int>(visual_debug::Transfer::Pq));
static_assert(static_cast<int>(viewer_core::Primaries::Bt2020) ==
              static_cast<int>(visual_debug::Primaries::Bt2020));

template <typename To, typename From> std::optional<To> converted(const std::optional<From>& value) {
    return value ? std::optional<To>(static_cast<To>(*value)) : std::nullopt;
}

template <class... F> struct Overloaded : F... {
    using F::operator()...;
};
template <class... F> Overloaded(F...) -> Overloaded<F...>;

viewer_core::Colour toCore(const visual_debug::Colour& colour) { return {colour.r, colour.g, colour.b}; }
visual_debug::Colour toWire(const viewer_core::Colour& colour) { return {colour.r, colour.g, colour.b}; }

viewer_core::Overlay toCore(const visual_debug::Overlay& overlay) {
    return std::visit(Overloaded{
                          [](const visual_debug::Polygon& s) -> viewer_core::Overlay {
                              return viewer_core::Polygon{s.points, s.label, toCore(s.colour)};
                          },
                          [](const visual_debug::Rectangle& s) -> viewer_core::Overlay {
                              return viewer_core::Rectangle{s.rect, s.label, toCore(s.colour)};
                          },
                          [](const visual_debug::Circle& s) -> viewer_core::Overlay {
                              return viewer_core::Circle{s.centre, s.radius, s.label, toCore(s.colour)};
                          },
                          [](const visual_debug::Point& s) -> viewer_core::Overlay {
                              return viewer_core::Point{s.at, s.label, toCore(s.colour)};
                          },
                      },
                      overlay);
}

visual_debug::Overlay toWire(const viewer_core::Overlay& overlay) {
    return std::visit(Overloaded{
                          [](const viewer_core::Polygon& s) -> visual_debug::Overlay {
                              return visual_debug::Polygon{s.points, s.label, toWire(s.colour)};
                          },
                          [](const viewer_core::Rectangle& s) -> visual_debug::Overlay {
                              return visual_debug::Rectangle{s.rect, s.label, toWire(s.colour)};
                          },
                          [](const viewer_core::Circle& s) -> visual_debug::Overlay {
                              return visual_debug::Circle{s.centre, s.radius, s.label, toWire(s.colour)};
                          },
                          [](const viewer_core::Point& s) -> visual_debug::Overlay {
                              return visual_debug::Point{s.at, s.label, toWire(s.colour)};
                          },
                      },
                      overlay);
}

} // namespace

viewer_core::ImageTags toImageTags(const visual_debug::ImageDescription& description) {
    std::vector<viewer_core::Overlay> overlays;
    overlays.reserve(description.overlays.size());
    for (const visual_debug::Overlay& overlay : description.overlays) {
        overlays.push_back(toCore(overlay));
    }
    return {.valueRange = {description.valueRange.min, description.valueRange.max},
            .transfer = converted<viewer_core::Transfer>(description.transfer),
            .primaries = converted<viewer_core::Primaries>(description.primaries),
            .overlays = viewer_core::makeOverlays(std::move(overlays))};
}

visual_debug::ImageDescription toDescription(const viewer_core::ImageTags& tags) {
    visual_debug::ImageDescription description{.valueRange = {tags.valueRange.min, tags.valueRange.max},
                                               .transfer = converted<visual_debug::Transfer>(tags.transfer),
                                               .primaries =
                                                   converted<visual_debug::Primaries>(tags.primaries)};
    description.overlays.reserve(tags.overlayList().size());
    for (const viewer_core::Overlay& overlay : tags.overlayList()) {
        description.overlays.push_back(toWire(overlay));
    }
    return description;
}
