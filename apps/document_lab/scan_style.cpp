#include "scan_style.h"

namespace document_lab {

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

} // namespace document_lab
