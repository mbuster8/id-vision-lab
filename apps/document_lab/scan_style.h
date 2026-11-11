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

// WCAG 2.x's contrast ratio of two opaque colours, 1 to 21.
double contrastRatio(const QColor& foreground, const QColor& background);
// `top` over an opaque `bottom`, by `top`'s alpha.
QColor over(const QColor& top, const QColor& bottom);

} // namespace document_lab
