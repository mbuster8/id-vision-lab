#pragma once

// The truth doc_vision_eval compares reads with (A4.1, spec §6.10): a truth file's or MIDV-2020's template's
// lines, in the form a read is compared in. Never printed: the owner's truth file is their document's MRZ.

#include <doc_vision/mrz.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace test_support {

// Whitespace removed and upper-cased, then parsed as the reader's lines are (so an empty field's check digit
// compares as the parser writes it): two lines of 44 (TD3), two of 36 (TD2) or three of 30 (TD1). Anything
// else, or lines the parser refuses, is nothing.
inline std::optional<doc_vision::Mrz> truthOf(const std::vector<std::string>& lines) {
    std::vector<std::string> clean;
    for (const std::string& line : lines) {
        std::string out;
        for (const char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        clean.push_back(std::move(out));
    }
    std::size_t length = 0;
    if (clean.size() == 2 && (clean[0].size() == 44 || clean[0].size() == 36)) {
        length = clean[0].size();
    } else if (clean.size() == 3) {
        length = 30;
    }
    if (length == 0) {
        return std::nullopt;
    }
    for (const std::string& line : clean) {
        if (line.size() != length) {
            return std::nullopt;
        }
    }
    return doc_vision::parseMrz(clean);
}

// A truth file's lines that are not blank, as written (truthOf cleans them); none if it cannot be read.
inline std::vector<std::string> truthLinesIn(const std::filesystem::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        for (const char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                lines.push_back(line);
                break;
            }
        }
    }
    return lines;
}

} // namespace test_support
