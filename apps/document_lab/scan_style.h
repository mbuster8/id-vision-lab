#pragma once

#include <lab_core/frame_processor.h>

#include <QColor>

namespace document_lab {

// The results panel's colour for a finding's value in the light or the dark appearance: green for good news,
// red for bad (docs/images/scan-flow/02); invalid for a plain value, which keeps the palette's text colour.
QColor toneColour(lab_core::Finding::Tone tone, bool dark);

} // namespace document_lab
