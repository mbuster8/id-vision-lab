#pragma once

#include <QString>

namespace document_lab {

// Where the Document Lab's models are installed beside it: the bundle's Contents/Resources/models on macOS,
// else a "models" folder beside the program (cmake/models.cmake copies them there).
QString defaultModelFolder();

} // namespace document_lab
