#include "lab_core/document_kind.h"

#include "fake_mrz_reader.h"
#include "procedural_page.h"

#include <doc_vision/mrz.h>

#include <gtest/gtest.h>

// A4.1: what the document is, by its MRZ's format, for the words after the accept.

namespace {

using lab_core::DocumentKind;

doc_vision::Mrz parsed(const std::vector<std::string>& lines) {
    const std::optional<doc_vision::Mrz> mrz = doc_vision::parseMrz(lines);
    EXPECT_TRUE(mrz && mrz->valid());
    return mrz.value_or(doc_vision::Mrz{});
}

TEST(DocumentKind, ByFormat) {
    EXPECT_EQ(lab_core::documentKind(parsed({test_support::kSpecimenLine1, test_support::kSpecimenLine2})),
              DocumentKind::Passport);
    EXPECT_EQ(lab_core::documentKind(parsed(test_support::kTd1Specimen)), DocumentKind::IdCard);
    EXPECT_EQ(lab_core::documentKind(
                  parsed({"I<UTOERIKSSON<<ANNA<MARIA<<<<<<<<<<<", "D231458907UTO7408122F1204159<<<<<<<6"})),
              DocumentKind::IdCard);
}

} // namespace
