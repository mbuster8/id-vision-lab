#include "mrz_truth.h"

#include "scratch_folder.h"

#include <gtest/gtest.h>

#include <fstream>
#include <utility>

namespace {

using doc_vision::MrzFormat;
using test_support::truthLinesIn;
using test_support::truthOf;

// ICAO 9303's specimens: the passport (TD3), the TD2 document and the TD1 card's back.
const std::vector<std::string> kTd3{"P<UTOERIKSSON<<ANNA<MARIA<<<<<<<<<<<<<<<<<<<",
                                    "L898902C36UTO7408122F1204159ZE184226B<<<<<10"};
const std::vector<std::string> kTd2{"I<UTOERIKSSON<<ANNA<MARIA<<<<<<<<<<<",
                                    "D231458907UTO7408122F1204159<<<<<<<6"};
const std::vector<std::string> kTd1{"I<UTOD231458907<<<<<<<<<<<<<<<", "7408122F1204159UTO<<<<<<<<<<<6",
                                    "ERIKSSON<<ANNA<MARIA<<<<<<<<<<"};

TEST(MrzTruth, EachFormat) {
    for (const auto& [lines, format] : {std::pair{kTd3, MrzFormat::Td3}, std::pair{kTd2, MrzFormat::Td2},
                                        std::pair{kTd1, MrzFormat::Td1}}) {
        const std::optional<doc_vision::Mrz> truth = truthOf(lines);
        ASSERT_TRUE(truth) << lines.size() << " lines of " << lines[0].size();
        EXPECT_EQ(truth->format, format);
        EXPECT_EQ(truth->lines, lines);
    }
}

// As someone types a truth file: spaces, lower case, a Windows line end.
TEST(MrzTruth, SpacesAndCaseAreIgnored) {
    const std::optional<doc_vision::Mrz> truth = truthOf({" p<uto ERIKSSON<<ANNA<MARIA<<<<<<<<<<<<<<<<<<<\r",
                                                          "L898902C36UTO7408122F1204159ZE184226B<<<<<10 "});
    ASSERT_TRUE(truth);
    EXPECT_EQ(truth->lines, kTd3);
}

TEST(MrzTruth, Malformed) {
    EXPECT_FALSE(truthOf({kTd1[0], kTd1[1]}));                   // two lines of 30
    EXPECT_FALSE(truthOf({kTd3[0], kTd3[1].substr(0, 43)}));     // a short line
    EXPECT_FALSE(truthOf({kTd1[0], kTd1[1], kTd1[2], kTd1[2]})); // four lines
    EXPECT_FALSE(truthOf({kTd3[0], kTd2[1]}));                   // 44 and 36
    EXPECT_FALSE(truthOf({}));
}

TEST(MrzTruth, LinesInAFile) {
    const test_support::ScratchFolder folder;
    const std::filesystem::path file = folder.path() / "truth.txt";
    {
        std::ofstream out(file, std::ios::binary);
        out << kTd1[0] << "\r\n\r\n" << kTd1[1] << "\n   \n" << kTd1[2] << "\n";
    }

    const std::vector<std::string> lines = truthLinesIn(file);

    ASSERT_EQ(lines.size(), 3U);
    const std::optional<doc_vision::Mrz> truth = truthOf(lines);
    ASSERT_TRUE(truth);
    EXPECT_EQ(truth->lines, kTd1);
    EXPECT_TRUE(truthLinesIn(folder.path() / "missing.txt").empty());
}

} // namespace
