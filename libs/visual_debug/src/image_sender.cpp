#include "visual_debug/image_sender.h"

namespace visual_debug {

std::string_view toString(SendResult result) {
    switch (result) {
    case SendResult::Delivered:
        return "Delivered";
    case SendResult::ViewerUnavailable:
        return "Viewer unavailable";
    case SendResult::Rejected:
        return "Rejected";
    case SendResult::InvalidImage:
        return "Invalid image";
    case SendResult::Timeout:
        return "Timeout";
    case SendResult::InvalidEndpoint:
        return "Invalid endpoint";
    }
    return "Unknown";
}

std::string_view toString(ViewerState state) {
    switch (state) {
    case ViewerState::Listening:
        return "listening";
    case ViewerState::Paused:
        return "paused";
    case ViewerState::PredatesPing:
        return "predates ping";
    case ViewerState::Unavailable:
        return "unavailable";
    case ViewerState::InvalidEndpoint:
        return "invalid endpoint";
    }
    return "unknown";
}

} // namespace visual_debug
