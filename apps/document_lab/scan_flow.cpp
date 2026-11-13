#include "scan_flow.h"

namespace document_lab {

std::optional<ScanStep> next(ScanState state, ScanEvent event) {
    // Leaving a scan always turns a front stage off (a no-op when none was on), before a resume's reset.
    const ScanStep backToGuiding{.to = ScanState::Guiding,
                                 .resume = true,
                                 .keepScan = false,
                                 .pause = Pause::None,
                                 .frontStage = false};
    const ScanStep runEnded{.to = ScanState::Guiding,
                            .resume = false,
                            .keepScan = false,
                            .pause = Pause::None,
                            .frontStage = false};
    const ScanStep toScanned{
        .to = ScanState::Scanned, .resume = false, .keepScan = true, .pause = Pause::All, .frontStage = {}};
    switch (state) {
    case ScanState::Guiding:
        if (event == ScanEvent::Accepted) {
            return toScanned;
        }
        if (event == ScanEvent::AcceptedTd1) {
            return ScanStep{.to = ScanState::TurnOver,
                            .resume = false,
                            .keepScan = true,
                            .pause = Pause::Replay,
                            .frontStage = true};
        }
        return std::nullopt;
    case ScanState::TurnOver:
        switch (event) {
        case ScanEvent::SourceChanged:
            return ScanStep{.to = ScanState::TurnOver,
                            .resume = false,
                            .keepScan = true,
                            .pause = Pause::None,
                            .frontStage = {}};
        case ScanEvent::FrontKept:
        case ScanEvent::SkipFront:
            return toScanned;
        case ScanEvent::ScanAgain:
            return backToGuiding;
        case ScanEvent::RunEnded:
            return runEnded;
        case ScanEvent::Accepted:
        case ScanEvent::AcceptedTd1:
        case ScanEvent::SkipChip:
        case ScanEvent::Discard:
        case ScanEvent::ScanAnother:
            break;
        }
        return std::nullopt;
    case ScanState::Scanned:
        switch (event) {
        case ScanEvent::SkipChip:
            return ScanStep{.to = ScanState::Review,
                            .resume = false,
                            .keepScan = true,
                            .pause = Pause::None,
                            .frontStage = {}};
        case ScanEvent::ScanAgain:
            return backToGuiding;
        case ScanEvent::RunEnded:
            return runEnded;
        case ScanEvent::Accepted:
        case ScanEvent::AcceptedTd1:
        case ScanEvent::FrontKept:
        case ScanEvent::SkipFront:
        case ScanEvent::SourceChanged:
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
        case ScanEvent::AcceptedTd1:
        case ScanEvent::FrontKept:
        case ScanEvent::SkipFront:
        case ScanEvent::SourceChanged:
        case ScanEvent::SkipChip:
        case ScanEvent::ScanAgain:
            break;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<ScanEvent> eventFor(ScanState state, ScanKey key) {
    switch (state) {
    case ScanState::TurnOver:
        return key == ScanKey::Enter ? ScanEvent::SkipFront : ScanEvent::ScanAgain;
    case ScanState::Scanned:
        return key == ScanKey::Enter ? ScanEvent::SkipChip : ScanEvent::ScanAgain;
    case ScanState::Guiding:
    case ScanState::Review:
        break;
    }
    return std::nullopt;
}

} // namespace document_lab
