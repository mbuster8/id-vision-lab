#include "doc_vision/mrz.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// ICAO Doc 9303's public "Utopia" specimens (Part 4 for TD3, Part 5 for TD1, Part 6 for TD2): no real person.

namespace {

using doc_vision::checkDigit;
using doc_vision::Mrz;
using doc_vision::MrzCheck;
using doc_vision::MrzCheckResult;
using doc_vision::MrzDate;
using doc_vision::MrzFormat;
using doc_vision::parseMrz;

std::string padded(std::string prefix, std::size_t width) {
    prefix.resize(width, '<');
    return prefix;
}

const std::string kTd3Line1 = padded("P<UTOERIKSSON<<ANNA<MARIA", 44);
const std::string kTd3Line2 = "L898902C36UTO7408122F1204159ZE184226B<<<<<10";
const std::string kTd2Line1 = padded("I<UTOERIKSSON<<ANNA<MARIA", 36);
const std::string kTd2Line2 = "D231458907UTO7408122F1204159<<<<<<<6";
const std::string kTd1Line1 = padded("I<UTOD231458907", 30);
const std::string kTd1Line2 = "7408122F1204159UTO<<<<<<<<<<<6";
const std::string kTd1Line3 = padded("ERIKSSON<<ANNA<MARIA", 30);

const MrzDate kToday{2026, 11, 12};

char digitOf(std::string_view field) { return static_cast<char>('0' + checkDigit(field)); }

// A TD3 line 2 with every check digit recomputed from its fields (the personal number's as `<` when it is
// empty).
std::string checkedTd3(std::string line) {
    line[9] = digitOf(line.substr(0, 9));
    line[19] = digitOf(line.substr(13, 6));
    line[27] = digitOf(line.substr(21, 6));
    if (line.substr(28, 14) != std::string(14, '<')) {
        line[42] = digitOf(line.substr(28, 14));
    }
    line[43] = digitOf(line.substr(0, 10) + line.substr(13, 7) + line.substr(21, 22));
    return line;
}

// A TD3 line 2 with only its composite recomputed.
std::string withTd3Composite(std::string line) {
    line[43] = digitOf(line.substr(0, 10) + line.substr(13, 7) + line.substr(21, 22));
    return line;
}

std::vector<MrzCheckResult> allPassed(bool optionalData) {
    std::vector<MrzCheckResult> checks{
        {MrzCheck::DocumentNumber, true}, {MrzCheck::BirthDate, true}, {MrzCheck::ExpiryDate, true}};
    if (optionalData) {
        checks.push_back({MrzCheck::OptionalData, true});
    }
    checks.push_back({MrzCheck::Composite, true});
    return checks;
}

bool passed(const Mrz& mrz, MrzCheck check) {
    for (const MrzCheckResult& result : mrz.checks) {
        if (result.check == check) {
            return result.passed;
        }
    }
    ADD_FAILURE() << "no such check";
    return false;
}

MrzDate date(int year, int month, int day) { return {year, month, day}; }

std::optional<MrzDate> birth(const std::string& yymmdd) {
    Mrz mrz;
    mrz.birthDate = yymmdd;
    return doc_vision::birthDateOf(mrz, kToday);
}

std::optional<MrzDate> expiry(const std::string& yymmdd) {
    Mrz mrz;
    mrz.expiryDate = yymmdd;
    return doc_vision::expiryDateOf(mrz, kToday);
}

TEST(MrzCheckDigit, SpecimenFields) {
    EXPECT_EQ(checkDigit("L898902C3"), 6);
    EXPECT_EQ(checkDigit("740812"), 2);
    EXPECT_EQ(checkDigit("120415"), 9);
    EXPECT_EQ(checkDigit("ZE184226B<<<<<"), 1);
    EXPECT_EQ(checkDigit("D23145890"), 7);
    EXPECT_EQ(checkDigit(""), 0);
    EXPECT_EQ(checkDigit("A-1"), -1);
    EXPECT_EQ(checkDigit("a"), -1);
}

TEST(MrzParse, Td3Specimen) {
    const std::optional<Mrz> mrz = parseMrz({kTd3Line1, kTd3Line2});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->format, MrzFormat::Td3);
    EXPECT_EQ(mrz->documentCode, "P");
    EXPECT_EQ(mrz->issuingState, "UTO");
    EXPECT_EQ(mrz->surname, "ERIKSSON");
    EXPECT_EQ(mrz->givenNames, "ANNA MARIA");
    EXPECT_EQ(mrz->documentNumber, "L898902C3");
    EXPECT_EQ(mrz->nationality, "UTO");
    EXPECT_EQ(mrz->birthDate, "740812");
    EXPECT_EQ(mrz->sex, "F");
    EXPECT_EQ(mrz->expiryDate, "120415");
    EXPECT_EQ(mrz->optionalData, "ZE184226B");
    EXPECT_EQ(mrz->checks, allPassed(true));
    EXPECT_TRUE(mrz->wellFormed);
    EXPECT_TRUE(mrz->valid());
    EXPECT_TRUE(mrz->passport());
    EXPECT_EQ(mrz->text(), kTd3Line1 + "\n" + kTd3Line2);
}

TEST(MrzParse, Td2Specimen) {
    const std::optional<Mrz> mrz = parseMrz({kTd2Line1, kTd2Line2});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->format, MrzFormat::Td2);
    EXPECT_EQ(mrz->documentCode, "I");
    EXPECT_EQ(mrz->issuingState, "UTO");
    EXPECT_EQ(mrz->surname, "ERIKSSON");
    EXPECT_EQ(mrz->givenNames, "ANNA MARIA");
    EXPECT_EQ(mrz->documentNumber, "D23145890");
    EXPECT_EQ(mrz->nationality, "UTO");
    EXPECT_EQ(mrz->birthDate, "740812");
    EXPECT_EQ(mrz->sex, "F");
    EXPECT_EQ(mrz->expiryDate, "120415");
    EXPECT_EQ(mrz->optionalData, "");
    EXPECT_EQ(mrz->checks, allPassed(false));
    EXPECT_TRUE(mrz->valid());
    EXPECT_FALSE(mrz->passport());
}

TEST(MrzParse, Td1Specimen) {
    const std::optional<Mrz> mrz = parseMrz({kTd1Line1, kTd1Line2, kTd1Line3});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->format, MrzFormat::Td1);
    EXPECT_EQ(mrz->documentCode, "I");
    EXPECT_EQ(mrz->issuingState, "UTO");
    EXPECT_EQ(mrz->surname, "ERIKSSON");
    EXPECT_EQ(mrz->givenNames, "ANNA MARIA");
    EXPECT_EQ(mrz->documentNumber, "D23145890");
    EXPECT_EQ(mrz->nationality, "UTO");
    EXPECT_EQ(mrz->birthDate, "740812");
    EXPECT_EQ(mrz->sex, "F");
    EXPECT_EQ(mrz->expiryDate, "120415");
    EXPECT_EQ(mrz->optionalData, "");
    EXPECT_EQ(mrz->checks, allPassed(false));
    EXPECT_TRUE(mrz->valid());
    EXPECT_FALSE(mrz->passport());
}

TEST(MrzParse, VisaIsNotAPassport) {
    std::string line1 = kTd3Line1;
    line1[0] = 'V';
    const std::optional<Mrz> mrz = parseMrz({line1, kTd3Line2});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->documentCode, "V");
    EXPECT_FALSE(mrz->passport());
}

// A specimen with another document code: no check digit covers the code, so it stays valid.
std::vector<std::string> withCode(std::vector<std::string> lines, std::string_view code) {
    lines[0].replace(0, 2, code);
    return lines;
}

// A4.1's D1: passports (TD3 `P`) and identity documents (TD1, TD2 `I`, `A`, `C`) are read; visas and every
// other pairing are not. Each refused read is valid, so the refusal is the code's, never an invalid read's.
TEST(MrzParse, SupportedDocuments) {
    const std::vector<std::string> td3{kTd3Line1, kTd3Line2};
    const std::vector<std::string> td2{kTd2Line1, kTd2Line2};
    const std::vector<std::string> td1{kTd1Line1, kTd1Line2, kTd1Line3};
    for (const auto& lines : {td3, td2, td1, withCode(td1, "AC"), withCode(td1, "C<"), withCode(td2, "A<")}) {
        const std::optional<Mrz> mrz = parseMrz(lines);
        ASSERT_TRUE(mrz && mrz->valid()) << lines[0];
        EXPECT_TRUE(mrz->supported()) << lines[0];
    }
    for (const auto& lines : {withCode(td3, "V<"), withCode(td2, "V<"), withCode(td1, "P<"),
                              withCode(td3, "I<"), withCode(td1, "V<")}) {
        const std::optional<Mrz> mrz = parseMrz(lines);
        ASSERT_TRUE(mrz && mrz->valid()) << lines[0];
        EXPECT_FALSE(mrz->supported()) << lines[0];
    }
}

TEST(MrzParse, EachCheckFails) {
    struct Case {
        std::size_t at;
        char to;
        MrzCheck check;
    };
    for (const Case& c : {Case{8, '4', MrzCheck::DocumentNumber}, Case{18, '3', MrzCheck::BirthDate},
                          Case{26, '6', MrzCheck::ExpiryDate}, Case{36, 'C', MrzCheck::OptionalData},
                          Case{43, '1', MrzCheck::Composite}}) {
        std::string line2 = kTd3Line2;
        line2[c.at] = c.to;
        const std::optional<Mrz> mrz = parseMrz({kTd3Line1, line2});
        ASSERT_TRUE(mrz) << line2;
        EXPECT_FALSE(passed(*mrz, c.check)) << line2;
        EXPECT_FALSE(passed(*mrz, MrzCheck::Composite)) << line2;
        EXPECT_FALSE(mrz->valid()) << line2;
    }
}

TEST(MrzParse, EmptyPersonalNumberMayCarryAFiller) {
    std::string line2 = kTd3Line2.substr(0, 28) + std::string(14, '<') + "<?";
    line2 = withTd3Composite(line2);
    EXPECT_EQ(line2[43], '8'); // 478, as the plan's review computed
    const std::optional<Mrz> mrz = parseMrz({kTd3Line1, line2});
    ASSERT_TRUE(mrz);
    EXPECT_TRUE(passed(*mrz, MrzCheck::OptionalData));
    EXPECT_TRUE(mrz->valid());
    EXPECT_EQ(mrz->optionalData, "");

    std::string filled = kTd3Line2;
    filled[42] = '<';
    const std::optional<Mrz> wrong = parseMrz({kTd3Line1, filled});
    ASSERT_TRUE(wrong);
    EXPECT_FALSE(passed(*wrong, MrzCheck::OptionalData));
}

TEST(MrzParse, EmptyPersonalNumbersCheckIsWrittenAsFiller) {
    const std::string line2 = withTd3Composite(kTd3Line2.substr(0, 28) + std::string(14, '<') + "<?");
    const std::optional<Mrz> filler = parseMrz({kTd3Line1, line2});
    ASSERT_TRUE(filler);
    for (const char read : {'0', 'O', 'Q', 'D', 'U'}) {
        std::string other = line2;
        other[42] = read;
        const std::optional<Mrz> mrz = parseMrz({kTd3Line1, other});
        ASSERT_TRUE(mrz) << read;
        EXPECT_EQ(mrz->text(), filler->text()) << read;
        EXPECT_EQ(mrz->lines[1][42], '<') << read;
        EXPECT_TRUE(mrz->valid()) << read;
    }
}

TEST(MrzParse, DigitFieldsTakeDigits) {
    const std::optional<Mrz> mrz = parseMrz({kTd3Line1, "L898902C36UTO74O8I2ZF I2O4IS9ZE184226B<<<<<IO"});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->lines[1], kTd3Line2);
    EXPECT_TRUE(mrz->valid());
}

TEST(MrzParse, LetterFieldsTakeLetters) {
    std::string line2 = kTd3Line2;
    line2[12] = '0';
    const std::optional<Mrz> mrz = parseMrz({padded("P<UT0ER1KSS0N<<ANNA<MAR1A", 44), line2});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->lines[0], kTd3Line1);
    EXPECT_EQ(mrz->nationality, "UTO");
    EXPECT_EQ(mrz->sex, "F");
    EXPECT_TRUE(mrz->valid());
}

TEST(MrzParse, MixedFieldsAreLeftAlone) {
    std::string line2 = kTd3Line2;
    line2[5] = 'O';
    const std::optional<Mrz> mrz = parseMrz({kTd3Line1, line2});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->documentNumber, "L8989O2C3");
    EXPECT_FALSE(passed(*mrz, MrzCheck::DocumentNumber));
    EXPECT_FALSE(mrz->valid());

    std::string personal = kTd3Line2;
    personal[30] = 'I'; // ZE1… → ZEI…: stays a letter
    const std::optional<Mrz> other = parseMrz({kTd3Line1, personal});
    ASSERT_TRUE(other);
    EXPECT_EQ(other->optionalData, "ZEI84226B");
}

TEST(MrzParse, LetterMatchingADigitModTenIsNotValid) {
    std::string line2 = kTd3Line2;
    line2[18] = 'C'; // 74081C: C = 12 ≡ 2, so check 2 and the composite still pass
    const std::optional<Mrz> letter = parseMrz({kTd3Line1, line2});
    ASSERT_TRUE(letter);
    for (const MrzCheckResult& check : letter->checks) {
        EXPECT_TRUE(check.passed);
    }
    EXPECT_FALSE(letter->wellFormed);
    EXPECT_FALSE(letter->valid());

    std::string unknownDay = kTd3Line2;
    unknownDay.replace(17, 2, "<<");
    unknownDay = checkedTd3(unknownDay);
    const std::optional<Mrz> partial = parseMrz({kTd3Line1, unknownDay});
    ASSERT_TRUE(partial);
    EXPECT_TRUE(partial->wellFormed);
    EXPECT_TRUE(partial->valid());

    std::string lone = kTd3Line2;
    lone[15] = '<'; // 74<812: < = 0 = the 0 it replaced, so every check passes
    const std::optional<Mrz> loneFiller = parseMrz({kTd3Line1, lone});
    ASSERT_TRUE(loneFiller);
    EXPECT_TRUE(passed(*loneFiller, MrzCheck::BirthDate));
    EXPECT_FALSE(loneFiller->wellFormed);
    EXPECT_FALSE(loneFiller->valid());
}

// Spec §1: substitutions whose values differ by a multiple of 10 pass the checks in a mixed field; only
// agreement across frames protects against them.
TEST(MrzParse, MixedFieldBlindSpotIsValid) {
    std::string line2 = kTd3Line2;
    line2[1] = 'S'; // 8 → S: 8 and 28
    const std::optional<Mrz> mrz = parseMrz({kTd3Line1, line2});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->documentNumber, "LS98902C3");
    EXPECT_TRUE(mrz->valid());
}

// Final review F3: no check digit covers the sex (nor the nationality), so a misread letter there would pass
// every check. Only F, M, X or '<' is a sex: anything else is not well formed.
TEST(MrzParse, SexOtherThanFmxIsNotWellFormed) {
    for (const char sex : {'F', 'M', 'X', '<'}) {
        std::string line2 = kTd3Line2;
        line2[20] = sex;
        const std::optional<Mrz> mrz = parseMrz({kTd3Line1, line2});
        ASSERT_TRUE(mrz) << sex;
        EXPECT_TRUE(mrz->valid()) << sex;
    }
    for (const char sex : {'E', 'N', 'A'}) {
        std::string line2 = kTd3Line2;
        line2[20] = sex;
        const std::optional<Mrz> mrz = parseMrz({kTd3Line1, line2});
        ASSERT_TRUE(mrz) << sex;
        EXPECT_FALSE(mrz->wellFormed) << sex;
        EXPECT_FALSE(mrz->valid()) << sex;
    }
}

TEST(MrzParse, LongDocumentNumberTd1) {
    const std::string line1 = "I<UTOD23145890<7349<<<<<<<<<<<";
    ASSERT_EQ(line1.size(), 30u);
    EXPECT_EQ(checkDigit("D23145890734"), 9);
    std::string line2 = kTd1Line2;
    line2[29] = digitOf(line1.substr(5, 25) + line2.substr(0, 7) + line2.substr(8, 7) + line2.substr(18, 11));
    const std::optional<Mrz> mrz = parseMrz({line1, line2, kTd1Line3});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->documentNumber, "D23145890734");
    EXPECT_EQ(mrz->optionalData, "");
    EXPECT_TRUE(passed(*mrz, MrzCheck::DocumentNumber));
    EXPECT_TRUE(mrz->wellFormed);
    EXPECT_TRUE(mrz->valid());
}

TEST(MrzParse, NamesSplitAndTrim) {
    const std::optional<Mrz> several =
        parseMrz({padded("P<UTOVAN<DER<BERG<<ANNA<MARIA<LOUISE", 44), kTd3Line2});
    ASSERT_TRUE(several);
    EXPECT_EQ(several->surname, "VAN DER BERG");
    EXPECT_EQ(several->givenNames, "ANNA MARIA LOUISE");

    const std::optional<Mrz> one = parseMrz({"P<UTO" + std::string(39, 'A'), kTd3Line2});
    ASSERT_TRUE(one);
    EXPECT_EQ(one->surname, std::string(39, 'A'));
    EXPECT_EQ(one->givenNames, "");
}

TEST(MrzParse, WrongShapes) {
    EXPECT_FALSE(parseMrz({kTd3Line1.substr(0, 43), kTd3Line2.substr(0, 43)}));
    EXPECT_FALSE(parseMrz({kTd3Line1 + "<", kTd3Line2 + "<"}));
    EXPECT_FALSE(parseMrz({kTd3Line1, kTd3Line2, kTd3Line2}));
    EXPECT_FALSE(parseMrz({kTd3Line1}));
    EXPECT_FALSE(parseMrz({kTd3Line1, kTd2Line2}));
    EXPECT_FALSE(parseMrz({}));
    std::string hash = kTd3Line2;
    hash[30] = '#';
    EXPECT_FALSE(parseMrz({kTd3Line1, hash}));

    std::string lower = " p<utoeriksson<<anna<maria";
    lower.resize(45, '<');
    const std::optional<Mrz> mrz = parseMrz({lower, "l898902c36uto7408122f1204159ze184226b<<<<<10\n"});
    ASSERT_TRUE(mrz);
    EXPECT_EQ(mrz->text(), kTd3Line1 + "\n" + kTd3Line2);
}

TEST(MrzDates, BirthCentury) {
    EXPECT_EQ(birth("740812"), date(1974, 8, 12));
    EXPECT_EQ(birth("251231"), date(2025, 12, 31));
    EXPECT_EQ(birth("261112"), date(2026, 11, 12));
    EXPECT_EQ(birth("261113"), date(1926, 11, 13));
    EXPECT_EQ(birth("270101"), date(1927, 1, 1));
}

TEST(MrzDates, ExpiryCentury) {
    EXPECT_EQ(expiry("120415"), date(2012, 4, 15));
    EXPECT_EQ(expiry("761112"), date(2076, 11, 12)); // exactly 50 years: not more
    EXPECT_EQ(expiry("761113"), date(1976, 11, 13));
    EXPECT_EQ(expiry("991231"), date(1999, 12, 31));
}

TEST(MrzDates, NotADate) {
    EXPECT_FALSE(birth("741312"));
    EXPECT_FALSE(birth("740230"));
    EXPECT_FALSE(birth("740012")); // "00" is not a month
    EXPECT_FALSE(birth("740800"));
    EXPECT_FALSE(expiry("12<415"));
    EXPECT_FALSE(expiry("12041"));
    EXPECT_EQ(birth("000229"), date(2000, 2, 29));
}

TEST(MrzDates, PartialBirthDate) {
    EXPECT_EQ(birth("7408<<"), date(1974, 8, 0));
    EXPECT_EQ(birth("2008<<"), date(2020, 8, 0));
    EXPECT_EQ(birth("74<<<<"), date(1974, 0, 0));
    EXPECT_EQ(birth("26<<<<"), date(2026, 0, 0)); // 1 January 2026 is not after today
}

// The encoder for the property test: every check digit computed with checkDigit.
std::string randomOf(std::mt19937& rng, std::string_view alphabet, std::size_t length) {
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        out += alphabet[rng() % alphabet.size()];
    }
    return out;
}

std::vector<std::string> encodeTd3(std::mt19937& rng, bool emptyPersonalNumber) {
    constexpr std::string_view kLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr std::string_view kDigits = "0123456789";
    constexpr std::string_view kAlphanumerics = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const std::string line1 =
        padded("P<UTO" + randomOf(rng, kLetters, 8) + "<<" + randomOf(rng, kLetters, 6), 44);
    std::string line2 = randomOf(rng, kAlphanumerics, 9) + "?" + "UTO" + randomOf(rng, kDigits, 6) + "?" +
                        (rng() % 2 == 0 ? "F" : "M") + randomOf(rng, kDigits, 6) + "?" +
                        (emptyPersonalNumber ? std::string(14, '<') : randomOf(rng, kAlphanumerics, 14)) +
                        (emptyPersonalNumber ? "<" : "?") + "?";
    return {line1, checkedTd3(line2)};
}

// Spec §5: in a digit-only field, a substitution is caught by its check, by correction, or by the well-formed
// rule; never a valid read that differs from the printed MRZ.
TEST(MrzParse, DigitFieldCorruptionNeverValidAndWrong) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789<";
    std::mt19937 rng(2026);
    std::vector<std::size_t> positions{9, 27, 42, 43};
    for (std::size_t i = 13; i <= 19; ++i) {
        positions.push_back(i);
    }
    for (std::size_t i = 21; i <= 26; ++i) {
        positions.push_back(i);
    }
    for (int n = 0; n < 50; ++n) {
        const std::vector<std::string> lines = encodeTd3(rng, n % 5 == 0);
        const std::optional<Mrz> original = parseMrz(lines);
        ASSERT_TRUE(original && original->valid()) << lines[1];
        for (const std::size_t at : positions) {
            for (const char c : kAlphabet) {
                if (c == lines[1][at]) {
                    continue;
                }
                std::string line2 = lines[1];
                line2[at] = c;
                const std::optional<Mrz> read = parseMrz({lines[0], line2});
                ASSERT_TRUE(read);
                EXPECT_TRUE(!read->valid() || read->text() == original->text())
                    << "valid and wrong: " << line2 << " from " << lines[1];
            }
        }
    }
}

} // namespace
