#include "doc_vision/mrz.h"

#include <algorithm>
#include <cstddef>

namespace doc_vision {

namespace {

constexpr char kFiller = '<';

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return c >= 'A' && c <= 'Z'; }
bool isMrzCharacter(char c) { return isDigit(c) || isLetter(c) || c == kFiller; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

char toDigit(char c) {
    switch (c) {
    case 'O':
    case 'Q':
    case 'D':
    case 'U':
        return '0';
    case 'I':
        return '1';
    case 'Z':
        return '2';
    case 'A':
        return '4';
    case 'S':
        return '5';
    case 'G':
        return '6';
    case 'B':
        return '8';
    default:
        return c;
    }
}

char toLetter(char c) {
    switch (c) {
    case '0':
        return 'O';
    case '1':
        return 'I';
    case '2':
        return 'Z';
    case '4':
        return 'A';
    case '5':
        return 'S';
    case '6':
        return 'G';
    case '8':
        return 'B';
    default:
        return c;
    }
}

void digits(std::string& line, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i) {
        line[i] = toDigit(line[i]);
    }
}

void letters(std::string& line, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i) {
        line[i] = toLetter(line[i]);
    }
}

bool allOf(std::string_view field, bool (*test)(char)) {
    return std::all_of(field.begin(), field.end(), test);
}
bool allFiller(std::string_view field) {
    return std::all_of(field.begin(), field.end(), [](char c) { return c == kFiller; });
}
bool lettersOrFiller(std::string_view field) {
    return std::all_of(field.begin(), field.end(), [](char c) { return isLetter(c) || c == kFiller; });
}
// ICAO 9303's sexes: F, M, X, or '<' when unspecified. No check digit covers the sex, so a misread letter is
// caught here or not at all.
bool isSex(char c) { return c == 'F' || c == 'M' || c == 'X' || c == kFiller; }

// A birth date's six characters: the year's two digits, then the month and the day, each two digits or `<<`.
bool birthWellFormed(std::string_view date) {
    const auto pair = [&](std::size_t at) {
        return (isDigit(date[at]) && isDigit(date[at + 1])) ||
               (date[at] == kFiller && date[at + 1] == kFiller);
    };
    return date.size() == 6 && isDigit(date[0]) && isDigit(date[1]) && pair(2) && pair(4);
}

bool checkPasses(std::string_view field, char digit) {
    const int expected = checkDigit(field);
    return expected >= 0 && isDigit(digit) && digit - '0' == expected;
}

// An optional field's check digit, which may be '<' when the field is empty.
bool optionalCheckPasses(std::string_view field, char digit) {
    return digit == kFiller ? allFiller(field) : checkPasses(field, digit);
}

std::string trimmed(std::string_view field) {
    const std::size_t first = field.find_first_not_of(kFiller);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = field.find_last_not_of(kFiller);
    return std::string(field.substr(first, last - first + 1));
}

// '<' as one space between words, fillers at either end dropped.
std::string words(std::string_view field) {
    std::string out;
    bool gap = false;
    for (const char c : field) {
        if (c == kFiller) {
            gap = !out.empty();
            continue;
        }
        if (gap) {
            out += ' ';
            gap = false;
        }
        out += c;
    }
    return out;
}

void splitNames(std::string_view field, Mrz& mrz) {
    const std::size_t split = field.find("<<");
    if (split == std::string_view::npos) {
        mrz.surname = words(field);
        mrz.givenNames.clear();
        return;
    }
    mrz.surname = words(field.substr(0, split));
    mrz.givenNames = words(field.substr(split + 2));
}

// TD1's and TD2's document number longer than 9 characters (Doc 9303 Parts 5 and 6): the check digit's place
// holds '<' and the number goes on in the optional data up to its first '<'; that run's last character is the
// number's check digit. `optional` is what follows the run.
struct Number {
    std::string number;
    char check = kFiller;
    std::string_view optional;
    bool longNumber = false;
};

Number numberOf(std::string_view nine, char checkPlace, std::string_view optional) {
    if (checkPlace != kFiller || allFiller(optional)) {
        return {std::string(nine), checkPlace, optional, false};
    }
    const std::size_t end = std::min(optional.find(kFiller), optional.size());
    if (end == 0) {
        return {std::string(nine), checkPlace, optional, false};
    }
    std::string number(nine);
    number += optional.substr(0, end - 1);
    return {std::move(number), optional[end - 1], optional.substr(end), true};
}

void addCheck(Mrz& mrz, MrzCheck check, bool passed) { mrz.checks.push_back({check, passed}); }

std::optional<Mrz> parseTd3(std::string line1, std::string line2) {
    letters(line1, 0, 44);
    digits(line2, 9, 10);
    letters(line2, 10, 13);
    digits(line2, 13, 20);
    letters(line2, 20, 21);
    digits(line2, 21, 28);
    const bool emptyPersonal = allFiller(std::string_view(line2).substr(28, 14));
    if (emptyPersonal) {
        const char check = toDigit(line2[42]);
        line2[42] = check == '0' ? kFiller : check;
    } else if (line2[42] != kFiller) {
        digits(line2, 42, 43);
    }
    digits(line2, 43, 44);

    Mrz mrz;
    mrz.format = MrzFormat::Td3;
    const std::string_view l1(line1);
    const std::string_view l2(line2);
    mrz.documentCode = trimmed(l1.substr(0, 2));
    mrz.issuingState = trimmed(l1.substr(2, 3));
    splitNames(l1.substr(5), mrz);
    mrz.documentNumber = trimmed(l2.substr(0, 9));
    mrz.nationality = trimmed(l2.substr(10, 3));
    mrz.birthDate = std::string(l2.substr(13, 6));
    mrz.sex = l2[20] == kFiller ? std::string() : std::string(1, l2[20]);
    mrz.expiryDate = std::string(l2.substr(21, 6));
    mrz.optionalData = trimmed(l2.substr(28, 14));

    addCheck(mrz, MrzCheck::DocumentNumber, checkPasses(l2.substr(0, 9), l2[9]));
    addCheck(mrz, MrzCheck::BirthDate, checkPasses(l2.substr(13, 6), l2[19]));
    addCheck(mrz, MrzCheck::ExpiryDate, checkPasses(l2.substr(21, 6), l2[27]));
    addCheck(mrz, MrzCheck::OptionalData, optionalCheckPasses(l2.substr(28, 14), l2[42]));
    const std::string composite =
        std::string(l2.substr(0, 10)) + std::string(l2.substr(13, 7)) + std::string(l2.substr(21, 22));
    addCheck(mrz, MrzCheck::Composite, checkPasses(composite, l2[43]));

    mrz.wellFormed = lettersOrFiller(l1) && isDigit(l2[9]) && lettersOrFiller(l2.substr(10, 3)) &&
                     birthWellFormed(l2.substr(13, 6)) && isDigit(l2[19]) && isSex(l2[20]) &&
                     allOf(l2.substr(21, 6), isDigit) && isDigit(l2[27]) &&
                     (isDigit(l2[42]) || (l2[42] == kFiller && emptyPersonal)) && isDigit(l2[43]);
    mrz.lines = {std::move(line1), std::move(line2)};
    return mrz;
}

std::optional<Mrz> parseTd2(std::string line1, std::string line2) {
    letters(line1, 0, 36);
    digits(line2, 9, 10);
    letters(line2, 10, 13);
    digits(line2, 13, 20);
    letters(line2, 20, 21);
    digits(line2, 21, 28);
    digits(line2, 35, 36);

    Mrz mrz;
    mrz.format = MrzFormat::Td2;
    const std::string_view l1(line1);
    const std::string_view l2(line2);
    mrz.documentCode = trimmed(l1.substr(0, 2));
    mrz.issuingState = trimmed(l1.substr(2, 3));
    splitNames(l1.substr(5), mrz);
    const Number number = numberOf(l2.substr(0, 9), l2[9], l2.substr(28, 7));
    mrz.documentNumber = trimmed(number.number);
    mrz.nationality = trimmed(l2.substr(10, 3));
    mrz.birthDate = std::string(l2.substr(13, 6));
    mrz.sex = l2[20] == kFiller ? std::string() : std::string(1, l2[20]);
    mrz.expiryDate = std::string(l2.substr(21, 6));
    mrz.optionalData = trimmed(number.optional);

    addCheck(mrz, MrzCheck::DocumentNumber, checkPasses(number.number, number.check));
    addCheck(mrz, MrzCheck::BirthDate, checkPasses(l2.substr(13, 6), l2[19]));
    addCheck(mrz, MrzCheck::ExpiryDate, checkPasses(l2.substr(21, 6), l2[27]));
    const std::string composite =
        std::string(l2.substr(0, 10)) + std::string(l2.substr(13, 7)) + std::string(l2.substr(21, 14));
    addCheck(mrz, MrzCheck::Composite, checkPasses(composite, l2[35]));

    mrz.wellFormed = lettersOrFiller(l1) && isDigit(number.check) && lettersOrFiller(l2.substr(10, 3)) &&
                     birthWellFormed(l2.substr(13, 6)) && isDigit(l2[19]) && isSex(l2[20]) &&
                     allOf(l2.substr(21, 6), isDigit) && isDigit(l2[27]) && isDigit(l2[35]);
    mrz.lines = {std::move(line1), std::move(line2)};
    return mrz;
}

std::optional<Mrz> parseTd1(std::string line1, std::string line2, std::string line3) {
    letters(line1, 0, 5);
    digits(line1, 14, 15);
    digits(line2, 0, 7);
    letters(line2, 7, 8);
    digits(line2, 8, 15);
    letters(line2, 15, 18);
    digits(line2, 29, 30);
    letters(line3, 0, 30);

    Mrz mrz;
    mrz.format = MrzFormat::Td1;
    const std::string_view l1(line1);
    const std::string_view l2(line2);
    const std::string_view l3(line3);
    mrz.documentCode = trimmed(l1.substr(0, 2));
    mrz.issuingState = trimmed(l1.substr(2, 3));
    const Number number = numberOf(l1.substr(5, 9), l1[14], l1.substr(15, 15));
    mrz.documentNumber = trimmed(number.number);
    mrz.birthDate = std::string(l2.substr(0, 6));
    mrz.sex = l2[7] == kFiller ? std::string() : std::string(1, l2[7]);
    mrz.expiryDate = std::string(l2.substr(8, 6));
    mrz.nationality = trimmed(l2.substr(15, 3));
    const std::string first = trimmed(number.optional);
    const std::string second = trimmed(l2.substr(18, 11));
    mrz.optionalData = first.empty() || second.empty() ? first + second : first + " " + second;
    splitNames(l3, mrz);

    addCheck(mrz, MrzCheck::DocumentNumber, checkPasses(number.number, number.check));
    addCheck(mrz, MrzCheck::BirthDate, checkPasses(l2.substr(0, 6), l2[6]));
    addCheck(mrz, MrzCheck::ExpiryDate, checkPasses(l2.substr(8, 6), l2[14]));
    const std::string composite = std::string(l1.substr(5, 25)) + std::string(l2.substr(0, 7)) +
                                  std::string(l2.substr(8, 7)) + std::string(l2.substr(18, 11));
    addCheck(mrz, MrzCheck::Composite, checkPasses(composite, l2[29]));

    mrz.wellFormed = lettersOrFiller(l1.substr(0, 5)) && isDigit(number.check) &&
                     birthWellFormed(l2.substr(0, 6)) && isDigit(l2[6]) && isSex(l2[7]) &&
                     allOf(l2.substr(8, 6), isDigit) && isDigit(l2[14]) &&
                     lettersOrFiller(l2.substr(15, 3)) && isDigit(l2[29]) && lettersOrFiller(l3);
    mrz.lines = {std::move(line1), std::move(line2), std::move(line3)};
    return mrz;
}

bool leapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysIn(int year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leapYear(year) ? 29 : kDays[month - 1];
}

std::optional<int> twoDigits(std::string_view text, std::size_t at) {
    if (!isDigit(text[at]) || !isDigit(text[at + 1])) {
        return std::nullopt;
    }
    return (text[at] - '0') * 10 + (text[at + 1] - '0');
}

bool realDay(const MrzDate& date) {
    return date.month >= 1 && date.month <= 12 && date.day >= 1 && date.day <= daysIn(date.year, date.month);
}

} // namespace

bool Mrz::valid() const {
    return wellFormed && !checks.empty() &&
           std::all_of(checks.begin(), checks.end(),
                       [](const MrzCheckResult& check) { return check.passed; });
}

bool Mrz::passport() const {
    return format == MrzFormat::Td3 && !documentCode.empty() && documentCode[0] == 'P';
}

bool Mrz::supported() const {
    if (passport()) {
        return true;
    }
    const bool identityFormat = format == MrzFormat::Td1 || format == MrzFormat::Td2;
    return identityFormat && !documentCode.empty() &&
           (documentCode[0] == 'I' || documentCode[0] == 'A' || documentCode[0] == 'C');
}

std::string Mrz::text() const {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) {
            out += '\n';
        }
        out += line;
    }
    return out;
}

int checkDigit(std::string_view field) {
    static constexpr int kWeights[3] = {7, 3, 1};
    int sum = 0;
    for (std::size_t i = 0; i < field.size(); ++i) {
        const char c = field[i];
        int value = 0;
        if (isDigit(c)) {
            value = c - '0';
        } else if (isLetter(c)) {
            value = c - 'A' + 10;
        } else if (c != kFiller) {
            return -1;
        }
        sum += value * kWeights[i % 3];
    }
    return sum % 10;
}

std::optional<Mrz> parseMrz(const std::vector<std::string>& lines) {
    std::vector<std::string> clean;
    clean.reserve(lines.size());
    for (const std::string& line : lines) {
        std::string out;
        for (const char c : line) {
            if (isSpace(c)) {
                continue;
            }
            const char upper = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            if (!isMrzCharacter(upper)) {
                return std::nullopt;
            }
            out += upper;
        }
        clean.push_back(std::move(out));
    }
    const auto all = [&](std::size_t length) {
        return std::all_of(clean.begin(), clean.end(),
                           [length](const std::string& line) { return line.size() == length; });
    };
    if (clean.size() == 2 && all(44)) {
        return parseTd3(std::move(clean[0]), std::move(clean[1]));
    }
    if (clean.size() == 2 && all(36)) {
        return parseTd2(std::move(clean[0]), std::move(clean[1]));
    }
    if (clean.size() == 3 && all(30)) {
        return parseTd1(std::move(clean[0]), std::move(clean[1]), std::move(clean[2]));
    }
    return std::nullopt;
}

std::optional<MrzDate> birthDateOf(const Mrz& mrz, MrzDate today) {
    const std::string_view text(mrz.birthDate);
    if (text.size() != 6 || !birthWellFormed(text)) {
        return std::nullopt;
    }
    const int yy = *twoDigits(text, 0);
    // `<<` is unknown (0); "00" is not a month or a day. An unknown month makes the day unknown too.
    const bool monthKnown = text[2] != kFiller;
    const bool dayKnown = monthKnown && text[4] != kFiller;
    const int month = monthKnown ? *twoDigits(text, 2) : 0;
    const int day = dayKnown ? *twoDigits(text, 4) : 0;
    if ((monthKnown && (month < 1 || month > 12)) || (dayKnown && (day < 1 || day > 31))) {
        return std::nullopt;
    }
    // The century whose earliest possible day is not after today.
    const MrzDate earliest{2000 + yy, month == 0 ? 1 : month, day == 0 ? 1 : day};
    const int year = earliest <= today ? 2000 + yy : 1900 + yy;
    const MrzDate date{year, month, day};
    if (month != 0 && day != 0 && !realDay(date)) {
        return std::nullopt;
    }
    return date;
}

std::optional<MrzDate> expiryDateOf(const Mrz& mrz, MrzDate today) {
    const std::string_view text(mrz.expiryDate);
    if (text.size() != 6 || !allOf(text, isDigit)) {
        return std::nullopt;
    }
    const int yy = *twoDigits(text, 0);
    const MrzDate later{2000 + yy, *twoDigits(text, 2), *twoDigits(text, 4)};
    const MrzDate limit{today.year + 50, today.month, today.day};
    const MrzDate date = later > limit ? MrzDate{1900 + yy, later.month, later.day} : later;
    if (!realDay(date)) {
        return std::nullopt;
    }
    return date;
}

} // namespace doc_vision
