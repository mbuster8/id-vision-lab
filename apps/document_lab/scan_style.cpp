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
