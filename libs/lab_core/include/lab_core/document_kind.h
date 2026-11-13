#pragma once

#include <doc_vision/mrz.h>

namespace lab_core {

// What an accepted MRZ says the document is (A4.1, spec §6.2): the outline cannot tell a card from a
// passport's page, so the words after the accept follow this. Qt-free, so Screen 04's rows and the window
// agree.
enum class DocumentKind { Passport, IdCard };

inline DocumentKind documentKind(const doc_vision::Mrz& mrz) {
    return mrz.format == doc_vision::MrzFormat::Td3 ? DocumentKind::Passport : DocumentKind::IdCard;
}

} // namespace lab_core
