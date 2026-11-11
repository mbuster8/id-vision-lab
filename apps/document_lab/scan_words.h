#pragma once

#include <doc_vision/guidance.h>
#include <doc_vision/quality_gates.h>

#include <QString>

namespace document_lab {

// What the banner says while the Document processor is chosen and no run gives results, or its model is
// unavailable: the window's own states, in the banner's place.
enum class WindowState { NoCamera, AccessOff, CameraStopped, Idle, ModelMissing, RuntimeMissing };

// The banner's two lines, its tone, and whether its icon is the ID card rather than the tone's ✓ or ✗.
struct BannerContent {
    doc_vision::Tone tone = doc_vision::Tone::Neutral;
    bool cardIcon = false;
    QString bigLine;
    QString smallLine; // empty: one line only

    bool operator==(const BannerContent&) const = default;
};

// A hint's banner (ADR-004's table, A3.1's rows).
BannerContent bannerFor(doc_vision::Hint hint);
// A window state's banner. `detail` is CameraStopped's small line, the failure's reason; ignored otherwise.
// AccessOff's small line is the running platform's.
BannerContent bannerFor(WindowState state, const QString& detail = {});

// A gate's chip: "In view", "Size", "Flat", "Sharp", "No glare", "Light".
QString chipLabel(doc_vision::Gate gate);

// The status bar's "State:" word for the shown hint: "searching", "adjust (sharpness)" (the gate to fix) or
// "good".
QString stateWord(doc_vision::Hint shown);

} // namespace document_lab
