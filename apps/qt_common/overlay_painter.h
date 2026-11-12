#pragma once

#include "viewer_core/overlay.h"

#include <QColor>
#include <QFont>
#include <QSizeF>
#include <QStaticText>
#include <QString>

#include <string>
#include <unordered_map>
#include <vector>

class QPainter;
class QTransform;

namespace qt_common {

// The look every overlay shares: a see-through fill and a solid edge in its colour, and a label on a solid
// tab. The viewer draws them; a later lab app showing the same overlays to its user can too.
inline constexpr double kOverlayFillOpacity = 0.25;
inline constexpr double kOverlayEdgeWidth = 2.0; // device-independent pixels, at any zoom
inline constexpr double kOverlayPointDiameter = 6.0;
// A Guide polygon: a line this wide on a black halo this wide and this opaque (0.55), so one device pixel of
// halo shows each side of the line over a light picture as over a dark one.
inline constexpr double kGuideWidth = 4.0;
inline constexpr double kGuideHaloWidth = 6.0;
inline constexpr int kGuideHaloAlpha = 140;
// A GuideDashed polygon (A4's MRZ band, screen 03): this far outside the shape, a line this wide in dashes
// this long with gaps this long, on a halo of the same dashes this wide, so the gaps show the picture.
inline constexpr double kDashedGuideOffset = 5.0;
inline constexpr double kDashedGuideWidth = 3.0;
inline constexpr double kDashedGuideHaloWidth = 5.0;
inline constexpr double kDashedGuideDash = 8.0;
inline constexpr double kDashedGuideGap = 6.0;

// Draws overlays given in image pixels; imageToWidget (a scale and a translation) maps them to the painter's
// coordinates. Shapes scale with it; edges, dots and label tabs keep their size on screen. Shapes entirely
// outside the painter's window are skipped.
void paintOverlays(QPainter& painter, const std::vector<viewer_core::Overlay>& overlays,
                   const QTransform& imageToWidget);

// paintOverlays for a view that paints again and again: it keeps each label laid out between paints, since
// laying out text is most of an overlay's cost and a live channel's labels repeat from frame to frame.
class OverlayPainter {
public:
    void paint(QPainter& painter, const std::vector<viewer_core::Overlay>& overlays,
               const QTransform& imageToWidget);

    struct Label {
        QStaticText text;
        QSizeF size; // of the text, without the tab's padding
    };

private:
    const Label& labelFor(const std::string& utf8, const QFont& font);

    std::unordered_map<std::string, Label> m_labels; // for m_font; bounded by kMaxLabels
    QFont m_font;
};

// Dark text when the colour's relative luminance (sRGB, as WCAG defines it) is above 0.179, where black and
// white contrast with it equally; light text below.
QColor labelTextColour(viewer_core::Colour colour);

// A label on one line: C0 and C1 control characters, U+007F, U+2028 and U+2029 become spaces.
QString singleLineLabel(const std::string& utf8);

} // namespace qt_common
