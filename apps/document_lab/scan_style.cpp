#include "scan_style.h"

#include <algorithm>
#include <cmath>

namespace document_lab {

namespace {

// WCAG's relative luminance of an sRGB colour.
double luminance(const QColor& colour) {
    const auto linear = [](double component) {
        return component <= 0.04045 ? component / 12.92 : std::pow((component + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(colour.redF()) + 0.7152 * linear(colour.greenF()) +
           0.0722 * linear(colour.blueF());
}

} // namespace

QColor toneColour(lab_core::Finding::Tone tone, bool dark) {
    switch (tone) {
    case lab_core::Finding::Tone::Good:
        return dark ? QColor(0x3D, 0xDC, 0x84) : QColor(0x1E, 0x7D, 0x45);
    case lab_core::Finding::Tone::Bad:
        return dark ? QColor(0xFF, 0x6B, 0x5E) : QColor(0xC2, 0x39, 0x2E);
    case lab_core::Finding::Tone::Plain:
        break;
    }
    return {};
}

ToneColours bannerColours(doc_vision::Tone tone) {
    switch (tone) {
    case doc_vision::Tone::Bad:
        return {kGuideBad, QColor(0xF2, 0xC7, 0xC2)};
    case doc_vision::Tone::Good:
        return {kGuideGood, QColor(0xC7, 0xEE, 0xD8)};
    case doc_vision::Tone::Neutral:
        break;
    }
    return {QColor(0xFF, 0xFF, 0xFF), QColor(0xC9, 0xCE, 0xD6)};
}

ReviewColours reviewColours(bool dark) {
    if (dark) {
        return {.title = QColor(0x3D, 0xDC, 0x84),
                .text = QColor(0xE8, 0xEA, 0xED),
                .secondary = QColor(0xA9, 0xB1, 0xBC),
                .cardBackground = QColor(0x24, 0x28, 0x2E),
                .pillBackground = QColor(0x2E, 0x33, 0x39),
                .checkCircle = QColor(0x1E, 0x3A, 0x2A),
                .checkMark = QColor(0x3D, 0xDC, 0x84),
                .warnBackground = QColor(0x35, 0x2A, 0x12),
                .warnText = QColor(0xF4, 0xC6, 0x7A),
                .discardBorder = QColor(0x6B, 0x3A, 0x35),
                .discardText = QColor(0xFF, 0x8A, 0x80),
                .discardBackground = QColor(0x3A, 0x22, 0x20),
                .primary = QColor(0x2F, 0x6F, 0xED),
                .primaryText = QColor(0xFF, 0xFF, 0xFF),
                .buttonBorder = QColor(0x4A, 0x50, 0x58),
                .link = QColor(0x8A, 0xB4, 0xFF),
                .reader = QColor(0x4A, 0x50, 0x58)};
    }
    return {.title = QColor(0x1E, 0x7D, 0x45),
            .text = QColor(0x1E, 0x23, 0x29),
            .secondary = QColor(0x4E, 0x57, 0x63),
            .cardBackground = QColor(0xFF, 0xFF, 0xFF),
            .pillBackground = QColor(0xF2, 0xF4, 0xF6),
            .checkCircle = QColor(0xE3, 0xF7, 0xEA),
            .checkMark = QColor(0x1E, 0x7D, 0x45),
            .warnBackground = QColor(0xFF, 0xF4, 0xE0),
            .warnText = QColor(0x7A, 0x4B, 0x00),
            .discardBorder = QColor(0xE3, 0xB2, 0xAC),
            .discardText = QColor(0xA8, 0x26, 0x1B),
            .discardBackground = QColor(0xFF, 0xFF, 0xFF),
            .primary = QColor(0x2F, 0x6F, 0xED),
            .primaryText = QColor(0xFF, 0xFF, 0xFF),
            .buttonBorder = QColor(0xC6, 0xCB, 0xD2),
            .link = QColor(0x1D, 0x4F, 0xB8),
            .reader = QColor(0x3B, 0x40, 0x48)};
}

double contrastRatio(const QColor& foreground, const QColor& background) {
    const double a = luminance(foreground);
    const double b = luminance(background);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

QColor over(const QColor& top, const QColor& bottom) {
    const double alpha = top.alphaF();
    const auto mix = [alpha](int a, int b) {
        return static_cast<int>(std::lround(alpha * a + (1 - alpha) * b));
    };
    return {mix(top.red(), bottom.red()), mix(top.green(), bottom.green()), mix(top.blue(), bottom.blue())};
}

} // namespace document_lab
