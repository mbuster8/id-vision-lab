#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace doc_vision {

// The machine-readable zone's three sizes (ICAO Doc 9303): TD1, 3 × 30 (ID cards, the MRZ on the back); TD2,
// 2 × 36; TD3, 2 × 44 (passports' data pages).
enum class MrzFormat { Td1, Td2, Td3 };

enum class MrzCheck { DocumentNumber, BirthDate, ExpiryDate, OptionalData, Composite };

struct MrzCheckResult {
    MrzCheck check = MrzCheck::DocumentNumber;
    bool passed = false;
    bool operator==(const MrzCheckResult&) const = default;
};

// A date; month or day 0 when unknown (a birth date may print `<<` for either).
struct MrzDate {
    int year = 0;
    int month = 0;
    int day = 0;
    auto operator<=>(const MrzDate&) const = default;
};

// An MRZ as read, after field-aware correction, and its fields.
struct Mrz {
    MrzFormat format = MrzFormat::Td3;
    std::vector<std::string> lines; // after correction
    std::string documentCode;       // fillers trimmed: "P", "I", "AC"
    std::string issuingState;       // fillers trimmed: "UTO", "D"
    std::string surname;            // '<' as spaces, fillers trimmed
    std::string givenNames;
    std::string documentNumber; // fillers trimmed; TD1's and TD2's long number joined
    std::string nationality;
    std::string birthDate; // YYMMDD as printed
    std::string sex;       // "F", "M", "X", or "" for '<'
    std::string expiryDate;
    std::string optionalData; // TD3: the personal number; TD1: both fields, joined by a space; TD2: its one
    std::vector<MrzCheckResult> checks; // in MrzCheck's order; OptionalData only for TD3
    // Digits where only digits may be, letters where only letters may be: check digits cannot see a letter
    // whose value equals the digit modulo 10 (C = 12 for 2).
    bool wellFormed = false;

    bool valid() const;       // well-formed, and every check passed
    bool passport() const;    // TD3 with a document code starting with 'P'
    std::string text() const; // the lines joined by '\n': what agreement compares
};

// ICAO 9303's check digit: weights 7, 3, 1; '0'–'9' are 0–9, 'A'–'Z' 10–35, '<' 0. -1 for any other
// character.
int checkDigit(std::string_view field);

// Whitespace removed and letters upper-cased, then: nothing unless 2 × 44, 2 × 36 or 3 × 30 characters of
// A–Z, 0–9 and '<'. Digit-only fields (dates, check digits) take O Q D U → 0, I → 1, Z → 2, A → 4, S → 5, G →
// 6, B → 8; letter-only fields (codes, nationality, sex, names) the reverse; mixed fields (document number,
// optional data) are left alone. An empty TD3 personal number's check digit is written '<' (0 and '<' are
// both worth 0), so two reads that differ only there agree.
std::optional<Mrz> parseMrz(const std::vector<std::string>& lines);

// The birth date in the century that puts its earliest possible day latest without passing `today`; month or
// day 0 for a `<<` part. Nothing when it is not a date.
std::optional<MrzDate> birthDateOf(const Mrz& mrz, MrzDate today);
// The expiry date in 20YY, unless that is more than 50 years after `today` (then 19YY). Nothing when it is
// not a date.
std::optional<MrzDate> expiryDateOf(const Mrz& mrz, MrzDate today);

} // namespace doc_vision
