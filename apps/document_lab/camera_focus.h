#pragma once

// The camera's own focus, reached around Qt Multimedia, which offers none for the owner's cameras (A3.3,
// ADR-005): a UVC camera's focus controls through IOKit on macOS, a driver's through DirectShow on Windows,
// none elsewhere. Only this header and lab_core's focus_control.h are included beside <windows.h>.

#include "lab_core/focus_control.h"

#include <QByteArray>

#include <memory>

namespace document_lab {

// The focus control of the camera Qt names `qtCameraId`; null when this platform cannot reach a camera with
// such an id, decided from the id alone. The control opens its device on its first call, on the processing
// thread.
std::unique_ptr<lab_core::FocusControl> makeFocusControl(const QByteArray& qtCameraId);

} // namespace document_lab
