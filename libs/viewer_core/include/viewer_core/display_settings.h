#pragma once

#include <map>
#include <optional>
#include <string>

namespace viewer_core {

// How original values map to the 8-bit picture on screen. Auto stretches each image (or frame) from its own
// minimum to its maximum, or shows its sender's value range; Hold keeps the range captured when it was
// chosen; Fixed uses a typed range; Hdr tone-maps an HLG or PQ image for an SDR screen, and shows any other
// as Auto. New modes go at the end: the viewer's menus are indexed by these values.
enum class RangeMode { Auto, Hold, Fixed, Hdr };

// For single-channel images only; other channel counts are always shown in their own colours.
enum class ColourMap { Grey, Turbo, Viridis };

struct DisplayRange {
    double min = 0.0;
    double max = 0.0;
    bool operator==(const DisplayRange&) const = default;
};

struct DisplaySettings {
    RangeMode mode = RangeMode::Auto;
    DisplayRange range{}; // used by Hold and Fixed
    ColourMap colours = ColourMap::Grey;
    bool operator==(const DisplaySettings&) const = default;
};

// The settings of each image or live channel, by name, for the session only: nothing is saved. Every setting
// given is kept, the default included, so a choice made by hand wins over a default that depends on the
// image.
class DisplaySettingsStore {
public:
    DisplaySettings get(const std::string& name) const;                 // the default when never set
    std::optional<DisplaySettings> find(const std::string& name) const; // nothing when never set
    void set(const std::string& name, const DisplaySettings& settings);
    void forget(const std::string& name);

private:
    std::map<std::string, DisplaySettings> m_settings;
};

} // namespace viewer_core
