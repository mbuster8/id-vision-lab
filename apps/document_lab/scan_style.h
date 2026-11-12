#pragma once

#include <doc_vision/guidance.h>
#include <lab_core/frame_processor.h>

#include <QColor>

namespace document_lab {

// The results panel's colour for a finding's value in the light or the dark appearance: green for good news,
// red for bad (docs/images/scan-flow/02); invalid for a plain value, which keeps the palette's text colour.
QColor toneColour(lab_core::Finding::Tone tone, bool dark);

// The banner's colours for a tone, the same in both appearances, since its panel is always dark: the big line
// and its icon, and the small line. The chips use the big line's.
struct ToneColours {
    QColor big;
    QColor small;
};
ToneColours bannerColours(doc_vision::Tone tone);

// The banner's and the chips' panel: rgba(8, 10, 13, 0.85).
inline const QColor kBannerPanel{8, 10, 13, 217};
// The canvas's page outline, the banner's good and bad colours.
inline const QColor kGuideGood{0x3D, 0xDC, 0x84};
inline const QColor kGuideBad{0xFF, 0x6B, 0x5E};

// A4: the Scanned card's and Review's colours in the light or the dark appearance (spec §3.3–3.5,
// docs/images/scan-flow/04 and 06). The canvas's overlays and the banner are the same in both.
struct ReviewColours {
    QColor title;          // "Passport scanned", the pill's ✓
    QColor text;           // the card's and the panel's text
    QColor secondary;      // their smaller text
    QColor cardBackground; // the card, the review panel
    QColor pillBackground; // the reader line's pill, the bar above the canvas
    QColor checkCircle;    // the card's circle
    QColor checkMark;      // its ✓
    QColor warnBackground; // the expiry warning
    QColor warnText;
    QColor discardBorder;
    QColor discardText;
    QColor discardBackground;
    QColor primary; // Save to captures
    QColor primaryText;
    QColor buttonBorder; // the other buttons
    QColor link;         // blue text
    QColor reader;       // the illustration's reader
};
ReviewColours reviewColours(bool dark);

// The illustration's passport and LED, the reader line's dots, the same in both appearances.
inline const QColor kIllustrationPassport{0x7A, 0x2E, 0x35};
inline const QColor kIllustrationLed{0x3D, 0xDC, 0x84};
inline const QColor kReaderDotWaiting{0x8B, 0x93, 0x9E};
inline const QColor kReaderDotReady{0x2F, 0x6F, 0xED};
// Scanned dims the captured frame: rgba(8, 10, 13, 0.62).
inline const QColor kScannedDim{8, 10, 13, 158};
// Review's MRZ band, dashed amber; Reading's is the guide's green.
inline const QColor kBandAmber{0xF4, 0xBF, 0x4F};
// The banner's three-part progress bar.
inline const QColor kProgressFilled{0x3D, 0xDC, 0x84};
inline const QColor kProgressEmpty{0x3E, 0x4A, 0x44};

// WCAG 2.x's contrast ratio of two opaque colours, 1 to 21.
double contrastRatio(const QColor& foreground, const QColor& background);
// `top` over an opaque `bottom`, by `top`'s alpha.
QColor over(const QColor& top, const QColor& bottom);

} // namespace document_lab
