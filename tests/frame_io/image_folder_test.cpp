#include "frame_io/image_folder.h"

#include "latch.h"
#include "scratch_folder.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frame_io::ReadStatus;
using test_support::ScratchFolder;

void writePng(const ScratchFolder& folder, const std::string& name, int value) {
    ASSERT_TRUE(cv::imwrite(folder.file(name), cv::Mat(8, 8, CV_8UC3, cv::Scalar::all(value))));
}

std::vector<std::string> namesOf(const std::vector<std::filesystem::path>& files) {
    std::vector<std::string> names;
    for (const auto& file : files) {
        names.push_back(file.filename().string());
    }
    return names;
}

TEST(NaturalOrder, NumbersAsNumbers) {
    EXPECT_TRUE(frame_io::naturalLess("frame2.png", "frame10.png"));
    EXPECT_FALSE(frame_io::naturalLess("frame10.png", "frame2.png"));
    EXPECT_TRUE(frame_io::naturalLess("a.png", "b.png"));
    // The same number: the shorter run first, so the order is still total.
    EXPECT_TRUE(frame_io::naturalLess("x9", "x09"));
    EXPECT_FALSE(frame_io::naturalLess("x09", "x9"));
    EXPECT_FALSE(frame_io::naturalLess("same", "same"));
}

TEST(ImageFolder, ListsReadableImagesInNaturalOrder) {
    const ScratchFolder folder;
    writePng(folder, "10.png", 10);
    writePng(folder, "2.png", 2);
    ASSERT_TRUE(cv::imwrite(folder.file("1.tif"), cv::Mat(8, 8, CV_8UC1, cv::Scalar(1))));
    std::ofstream(folder.file("notes.txt")) << "not an image";

    EXPECT_EQ(namesOf(frame_io::imageFilesIn(folder.path())),
              (std::vector<std::string>{"1.tif", "2.png", "10.png"}));
}

// A folder has no rate of its own: its frames are played as if 30 a second, frame n at n × 1e9 / 30 ns.
TEST(ImageFolder, FramesThirtyAFrameApart) {
    const ScratchFolder folder;
    writePng(folder, "a.png", 0);
    writePng(folder, "b.png", 1);
    writePng(folder, "c.png", 2);
    std::string error;
    const auto source = frame_io::openImageFolder(folder.path(), error);
    ASSERT_NE(source, nullptr) << error;

    const std::vector<std::int64_t> expected{0, 33'333'333, 66'666'666};
    for (std::uint64_t number = 0; number < 3; ++number) {
        const frame_io::ReadResult result = source->next();
        ASSERT_EQ(result.status, ReadStatus::Frame) << result.error;
        EXPECT_EQ(result.frame.number, number);
        EXPECT_EQ(result.frame.at.count(), expected[number]);
        EXPECT_EQ(result.frame.image.at<cv::Vec3b>(0, 0)[0], number);
    }
    EXPECT_EQ(source->next().status, ReadStatus::End);
}

TEST(ImageFolder, RateIsThirty) {
    const ScratchFolder folder;
    writePng(folder, "a.png", 0);
    std::string error;
    const auto source = frame_io::openImageFolder(folder.path(), error);
    ASSERT_NE(source, nullptr) << error;

    EXPECT_EQ(source->frameRate(), std::optional<double>(30.0));
}

TEST(ImageFolder, SeekRestartAndCount) {
    const ScratchFolder folder;
    writePng(folder, "1.png", 10);
    writePng(folder, "2.png", 20);
    writePng(folder, "3.png", 30);
    std::string error;
    const auto source = frame_io::openImageFolder(folder.path(), error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->frameCount(), std::optional<std::uint64_t>(3));
    EXPECT_TRUE(source->canSeek());

    source->seek(2);
    EXPECT_EQ(source->next().frame.image.at<cv::Vec3b>(0, 0)[0], 30);
    ASSERT_TRUE(source->restart(error)) << error;
    const frame_io::ReadResult first = source->next();
    EXPECT_EQ(first.frame.number, 0U);
    EXPECT_EQ(first.frame.image.at<cv::Vec3b>(0, 0)[0], 10);
}

// The processing thread waits for a frame without blocking in next(): ready() says whether next() would
// return at once, and the wake is called from the reading thread when it turns true.
TEST(ImageFolder, ReadyAndWake) {
    test_support::Latch latch;
    const ScratchFolder folder;
    writePng(folder, "1.png", 10);
    std::string error;
    const auto source = frame_io::openImageFolder(folder.path(), error,
                                                  {.beforeRead = [&latch](std::uint64_t) { latch.hold(); }});
    ASSERT_NE(source, nullptr) << error;
    std::atomic<int> wakes{0};
    source->setWake([&wakes] { ++wakes; });
    ASSERT_TRUE(latch.waitUntilEntered());

    EXPECT_FALSE(source->ready());
    latch.release();

    EXPECT_EQ(source->next().status, ReadStatus::Frame);
    EXPECT_GE(wakes.load(), 1);
}

TEST(ImageFolder, ErrorIsNotTheEnd) {
    const ScratchFolder folder;
    writePng(folder, "1.png", 10);
    std::ofstream(folder.file("2.png"), std::ios::binary) << "not a PNG at all";
    writePng(folder, "3.png", 30);
    std::string error;
    const auto source = frame_io::openImageFolder(folder.path(), error);
    ASSERT_NE(source, nullptr) << error;

    EXPECT_EQ(source->next().status, ReadStatus::Frame);
    const frame_io::ReadResult broken = source->next();
    EXPECT_EQ(broken.status, ReadStatus::Error);
    EXPECT_NE(broken.error.find("2.png"), std::string::npos) << broken.error;
}

// A read that throws is an error at that frame, never the folder's end.
TEST(ImageFolder, ThrowingReadIsAnError) {
    const ScratchFolder folder;
    writePng(folder, "1.png", 10);
    writePng(folder, "2.png", 20);
    writePng(folder, "3.png", 30);
    std::string error;
    const auto source =
        frame_io::openImageFolder(folder.path(), error, {.beforeRead = [](std::uint64_t index) {
                                      if (index == 1) {
                                          throw std::runtime_error("disk gone");
                                      }
                                  }});
    ASSERT_NE(source, nullptr) << error;

    EXPECT_EQ(source->next().status, ReadStatus::Frame);
    const frame_io::ReadResult broken = source->next();
    EXPECT_EQ(broken.status, ReadStatus::Error);
    EXPECT_NE(broken.error.find("2.png"), std::string::npos) << broken.error;
}

TEST(ImageFolder, EmptyFolderDoesNotOpen) {
    const ScratchFolder folder;
    std::string error;

    EXPECT_EQ(frame_io::openImageFolder(folder.path(), error), nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(ImageFileSource, OneFrameThenEnd) {
    const ScratchFolder folder;
    writePng(folder, "one.png", 7);
    std::string error;
    const auto source = frame_io::openImageFile(folder.path() / "one.png", error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->frameCount(), std::optional<std::uint64_t>(1));
    EXPECT_EQ(source->describe(), "one.png");

    EXPECT_EQ(source->next().status, ReadStatus::Frame);
    EXPECT_EQ(source->next().status, ReadStatus::End);
    ASSERT_TRUE(source->restart(error));
    EXPECT_EQ(source->next().status, ReadStatus::Frame);
}

// One picture has no rate: the window shows no position for it.
TEST(ImageFileSource, HasNoRate) {
    const ScratchFolder folder;
    writePng(folder, "one.png", 7);
    std::string error;
    const auto source = frame_io::openImageFile(folder.path() / "one.png", error);
    ASSERT_NE(source, nullptr) << error;

    EXPECT_EQ(source->frameRate(), std::nullopt);
}

TEST(ImageFileSource, SixteenBitGetsANominalRange) {
    const ScratchFolder folder;
    ASSERT_TRUE(cv::imwrite(folder.file("deep.png"), cv::Mat(4, 4, CV_16UC1, cv::Scalar(1000))));
    std::string error;
    const auto source = frame_io::openImageFile(folder.path() / "deep.png", error);
    ASSERT_NE(source, nullptr) << error;

    const frame_io::ReadResult result = source->next();
    ASSERT_EQ(result.status, ReadStatus::Frame);
    EXPECT_EQ(result.frame.image.depth(), CV_16U);
    EXPECT_EQ(result.frame.tags.valueRange, (visual_debug::ValueRange{0, 65535}));
}

TEST(ImageFileSource, FloatIsRefused) {
    const ScratchFolder folder;
    ASSERT_TRUE(cv::imwrite(folder.file("float.tif"), cv::Mat(4, 4, CV_32FC1, cv::Scalar(0.5))));
    std::string error;

    EXPECT_EQ(frame_io::openImageFile(folder.path() / "float.tif", error), nullptr);
    EXPECT_NE(error.find("32F"), std::string::npos) << error;
}

} // namespace
