#include "camera_focus.h"

namespace document_lab {

// Linux and anything else: the banner only for now (V4L2's controls later; spec D3).
std::unique_ptr<lab_core::FocusControl> makeFocusControl(const QByteArray&) { return nullptr; }

} // namespace document_lab
