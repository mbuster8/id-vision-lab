#include "scan_flow.h"

namespace document_lab {

std::optional<ScanStep> next(ScanState state, ScanEvent event) {
    const ScanStep backToGuiding{.to = ScanState::Guiding, .resume = true, .keepScan = false};
    const ScanStep runEnded{.to = ScanState::Guiding, .resume = false, .keepScan = false};
    switch (state) {
    case ScanState::Guiding:
        if (event == ScanEvent::Accepted) {
            return ScanStep{.to = ScanState::Scanned, .resume = false, .keepScan = true};
        }
        return std::nullopt;
    case ScanState::Scanned:
        switch (event) {
        case ScanEvent::SkipChip:
            return ScanStep{.to = ScanState::Review, .resume = false, .keepScan = true};
        case ScanEvent::ScanAgain:
            return backToGuiding;
        case ScanEvent::RunEnded:
            return runEnded;
        case ScanEvent::Accepted:
        case ScanEvent::Discard:
        case ScanEvent::ScanAnother:
            break;
        }
        return std::nullopt;
    case ScanState::Review:
        switch (event) {
        case ScanEvent::Discard:
        case ScanEvent::ScanAnother:
            return backToGuiding;
        case ScanEvent::RunEnded:
            return runEnded;
        case ScanEvent::Accepted:
        case ScanEvent::SkipChip:
        case ScanEvent::ScanAgain:
            break;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<ScanEvent> eventFor(ScanState state, ScanKey key) {
    if (state != ScanState::Scanned) {
        return std::nullopt;
    }
    return key == ScanKey::Enter ? ScanEvent::SkipChip : ScanEvent::ScanAgain;
}

} // namespace document_lab
