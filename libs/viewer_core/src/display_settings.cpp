#include "viewer_core/display_settings.h"

namespace viewer_core {

DisplaySettings DisplaySettingsStore::get(const std::string& name) const {
    const auto found = m_settings.find(name);
    return found != m_settings.end() ? found->second : DisplaySettings{};
}

std::optional<DisplaySettings> DisplaySettingsStore::find(const std::string& name) const {
    const auto found = m_settings.find(name);
    return found != m_settings.end() ? std::optional(found->second) : std::nullopt;
}

// Grows only with the names the user changed.
void DisplaySettingsStore::set(const std::string& name, const DisplaySettings& settings) {
    m_settings[name] = settings;
}

void DisplaySettingsStore::forget(const std::string& name) { m_settings.erase(name); }

} // namespace viewer_core
