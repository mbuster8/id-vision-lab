#include "visual_debug/endpoint.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace visual_debug {

std::string defaultEndpoint() {
    std::string value;
#ifdef _WIN32
    // MSVC deprecates getenv (warning C4996) for _dupenv_s, which copies the value while it reads it.
    char* copy = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&copy, &length, kEndpointEnvironmentVariable) == 0 && copy != nullptr) {
        value = copy;
    }
    std::free(copy);
#else
    if (const char* raw = std::getenv(kEndpointEnvironmentVariable)) {
        value = raw;
    }
#endif
    if (value.empty()) {
        return std::string(kDefaultEndpoint);
    }
    return value;
}

bool isLoopbackEndpoint(std::string_view endpoint) {
    constexpr std::array<std::string_view, 4> kLoopbackPrefixes{"tcp://127.0.0.1:", "tcp://[::1]:", "ipc://",
                                                                "inproc://"};
    return std::any_of(kLoopbackPrefixes.begin(), kLoopbackPrefixes.end(),
                       [&](std::string_view prefix) { return endpoint.starts_with(prefix); });
}

} // namespace visual_debug
