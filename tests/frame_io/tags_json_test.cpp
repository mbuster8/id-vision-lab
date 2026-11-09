// frame_io's private file writing (src/tags_json.h): a write that fails only as the file closes, as a full
// disk makes it, is a failed write, and leaves no half-written file of its own behind.

#include "scratch_folder.h"
#include "tags_json.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using test_support::ScratchFolder;

TEST(TagsJson, FailedCloseRemovesTheFileItMade) {
    const ScratchFolder folder;
    const std::filesystem::path file = folder.path() / "000001.png";
    std::ofstream stream(file, std::ios::binary);
    stream << "partial";
    stream.setstate(std::ios::badbit); // as a flush that found the disk full leaves it
    std::string error;

    EXPECT_FALSE(frame_io::finishWrite(stream, file, true, error));

    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_NE(error.find("000001.png"), std::string::npos) << error;
}

TEST(TagsJson, FailedCloseKeepsAFileItDidNotMake) {
    const ScratchFolder folder;
    const std::filesystem::path file = folder.path() / "recording.json";
    std::ofstream(file) << "{}";
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.setstate(std::ios::badbit);
    std::string error;

    EXPECT_FALSE(frame_io::finishWrite(stream, file, false, error));

    EXPECT_TRUE(std::filesystem::exists(file));
}

TEST(TagsJson, GoodCloseIsAWrite) {
    const ScratchFolder folder;
    const std::filesystem::path file = folder.path() / "frame.png";
    std::ofstream stream(file, std::ios::binary);
    stream << "bytes";
    std::string error;

    EXPECT_TRUE(frame_io::finishWrite(stream, file, true, error)) << error;
    EXPECT_EQ(std::filesystem::file_size(file), 5U);
}

// A few bytes, so they sit in the stream's buffer and the device refuses them only when the file closes.
TEST(TagsJson, FullDeviceIsAFailedWrite) {
#if defined(__linux__)
    const std::filesystem::path device("/dev/full");
    std::error_code failed;
    const std::filesystem::perms before = std::filesystem::status(device, failed).permissions();
    ASSERT_FALSE(failed) << failed.message();
    std::string error;

    EXPECT_FALSE(frame_io::writeOwnerOnly(device, std::string("abc"), error));

    EXPECT_TRUE(std::filesystem::exists(device));
    EXPECT_EQ(std::filesystem::status(device).permissions(), before);
#else
    GTEST_SKIP() << "only Linux has /dev/full";
#endif
}

} // namespace
