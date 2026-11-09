#include "ffmpeg_clip.h"
#include "frame_io/ffmpeg_video.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using frame_io::FfmpegVideo;
using frame_io::VideoFrame;
using test_support::Clip;
using test_support::kClipFrames;
using test_support::ScratchFolder;
using test_support::writeClip;
using visual_debug::Primaries;
using visual_debug::Transfer;

std::unique_ptr<FfmpegVideo> openInSoftware(const std::string& path) {
    std::string error;
    std::unique_ptr<FfmpegVideo> video = FfmpegVideo::open(path, false, error);
    EXPECT_NE(video, nullptr) << error;
    return video;
}

std::vector<VideoFrame> readAll(FfmpegVideo& video) {
    std::vector<VideoFrame> frames;
    while (std::optional<VideoFrame> frame = video.read()) {
        frames.push_back(std::move(*frame));
    }
    return frames;
}

cv::Vec3w middleOf(const VideoFrame& frame) {
    return frame.image.at<cv::Vec3w>(frame.image.rows / 2, frame.image.cols / 2);
}

// Every channel of the frame's middle pixel, for a grey frame.
void expectGreyNear(const VideoFrame& frame, double expected, double tolerance) {
    const cv::Vec3w pixel = middleOf(frame);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(pixel[channel], expected, tolerance) << "channel " << channel;
    }
}

// (H1) Limited range keeps its codes' places on the full scale: 64 and 940 of 1023 land at 4100 and 60218,
// and the codes above white keep their values up to 65535 instead of clipping at white.
TEST(FfmpegVideo, KeepsTheHeadroomAboveWhite) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{.y = {64, 940, 1019, 1023}}));
    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
    ASSERT_NE(video, nullptr);

    const std::vector<VideoFrame> frames = readAll(*video);

    ASSERT_EQ(frames.size(), 4U);
    for (const VideoFrame& frame : frames) {
        EXPECT_EQ(frame.image.type(), CV_16UC3);
        EXPECT_EQ(frame.image.size(), cv::Size(64, 48));
    }
    expectGreyNear(frames[0], 4100, 100);
    expectGreyNear(frames[1], 60218, 300);
    const cv::Vec3w white = middleOf(frames[1]);
    const cv::Vec3w aboveWhite = middleOf(frames[2]);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_GT(aboveWhite[channel], white[channel]) << "channel " << channel;
        EXPECT_LT(aboveWhite[channel], 65535) << "channel " << channel;
        EXPECT_GE(middleOf(frames[3])[channel], 65530) << "channel " << channel;
    }
    EXPECT_NEAR(video->info().valueRange.min, 4100, 41);
    EXPECT_NEAR(video->info().valueRange.max, 60218, 602);
}

// BT.2020's matrix, limited range, on the code scale.
TEST(FfmpegVideo, ColoursFollowTheMatrix) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(
        writeClip(path, Clip{.matrix = AVCOL_SPC_BT2020_NCL, .y = {500, 500, 500, 500}, .u = 300, .v = 700}));
    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
    ASSERT_NE(video, nullptr);

    const std::optional<VideoFrame> frame = video->read();

    ASSERT_TRUE(frame.has_value());
    const cv::Vec3w bgr = middleOf(*frame);
    EXPECT_NEAR(bgr[0], 7040, 300);
    EXPECT_NEAR(bgr[1], 27400, 300);
    EXPECT_NEAR(bgr[2], 49220, 300);
}

TEST(FfmpegVideo, TagsAnHlgClip) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{.matrix = AVCOL_SPC_BT2020_NCL,
                                     .transfer = AVCOL_TRC_ARIB_STD_B67,
                                     .primaries = AVCOL_PRI_BT2020}));

    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);

    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->info().transfer, Transfer::Hlg);
    EXPECT_EQ(video->info().primaries, Primaries::Bt2020);
    EXPECT_EQ(video->info().bitDepth, 10);
    EXPECT_EQ(video->info().size, cv::Size(64, 48));
    EXPECT_NEAR(video->info().fps, 30.0, 0.5); // Matroska stores none: FFmpeg estimates it from 4 frames
    EXPECT_EQ(video->info().decoder, "software");
    EXPECT_EQ(video->info().dolbyVisionProfile, 0);
}

TEST(FfmpegVideo, TagsAPqClip) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{.matrix = AVCOL_SPC_BT2020_NCL,
                                     .transfer = AVCOL_TRC_SMPTE2084,
                                     .primaries = AVCOL_PRI_BT2020}));

    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);

    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->info().transfer, Transfer::Pq);
    EXPECT_EQ(video->info().primaries, Primaries::Bt2020);
}

TEST(FfmpegVideo, UntaggedIsSdrBt709) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{}));

    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);

    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->info().transfer, Transfer::Sdr);
    EXPECT_EQ(video->info().primaries, Primaries::Bt709);
}

// As FFmpeg's own player turns it: a phone's portrait video is stored on its side with a display matrix.
TEST(FfmpegVideo, AppliesTheRotation) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mov");
    ASSERT_TRUE(writeClip(path, Clip{.block = cv::Rect(0, 0, 8, 8), .rotation = 90.0}));
    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
    ASSERT_NE(video, nullptr);

    const std::optional<VideoFrame> frame = video->read();

    EXPECT_EQ(video->info().size, cv::Size(48, 64));
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->image.size(), cv::Size(48, 64));
    EXPECT_GT(frame->image.at<cv::Vec3w>(3, 44)[1], 50000); // top-right
    EXPECT_LT(frame->image.at<cv::Vec3w>(3, 3)[1], 10000);
    EXPECT_LT(frame->image.at<cv::Vec3w>(60, 3)[1], 10000);
    EXPECT_LT(frame->image.at<cv::Vec3w>(60, 44)[1], 10000);
}

TEST(FfmpegVideo, TimestampsFromTheFirstFrame) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{}));
    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
    ASSERT_NE(video, nullptr);

    const std::vector<VideoFrame> frames = readAll(*video);

    ASSERT_EQ(frames.size(), 4U);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::chrono::duration<double, std::milli> at = frames[i].at;
        EXPECT_NEAR(at.count(), static_cast<double>(i) * 1000.0 / 30.0, 1.0) << "frame " << i;
    }
}

// The frame count the file's header gives, which tells a file cut short from its end: MOV stores the count,
// Matroska a duration, which with the rate gives it.
TEST(FfmpegVideo, InfoHasTheFrameCount) {
    const ScratchFolder folder;
    for (const char* name : {"clip.mkv", "clip.mov"}) {
        const std::string path = folder.file(name);
        ASSERT_TRUE(writeClip(path, Clip{})) << name;
        const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
        ASSERT_NE(video, nullptr) << name;

        EXPECT_EQ(video->info().frameCount, std::optional<std::uint64_t>(kClipFrames)) << name;
    }
}

// An 8-bit video lands on the same scale: 16 and 235 of 255 at 4112 and 60395.
TEST(FfmpegVideo, EightBitSourceOnTheSameScale) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(
        writeClip(path, Clip{.format = AV_PIX_FMT_YUV420P, .y = {16, 235, 255, 255}, .u = 128, .v = 128}));
    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
    ASSERT_NE(video, nullptr);

    const std::vector<VideoFrame> frames = readAll(*video);

    ASSERT_EQ(frames.size(), 4U);
    EXPECT_EQ(video->info().bitDepth, 8);
    expectGreyNear(frames[0], 4112, 150);
    expectGreyNear(frames[1], 60395, 400);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_GE(middleOf(frames[2])[channel], 65530) << "channel " << channel;
    }
}

// swscale takes another path for 8-bit sources; the colours follow the matrix there too. The codes are a
// quarter of ColoursFollowTheMatrix's, so the same colour on the 8-bit code scale.
TEST(FfmpegVideo, EightBitColoursFollowTheMatrix) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{.format = AV_PIX_FMT_YUV420P,
                                     .matrix = AVCOL_SPC_BT2020_NCL,
                                     .y = {125, 125, 125, 125},
                                     .u = 75,
                                     .v = 175}));
    const std::unique_ptr<FfmpegVideo> video = openInSoftware(path);
    ASSERT_NE(video, nullptr);

    const std::optional<VideoFrame> frame = video->read();

    ASSERT_TRUE(frame.has_value());
    const cv::Vec3w bgr = middleOf(*frame);
    EXPECT_NEAR(bgr[0], 7070, 300);
    EXPECT_NEAR(bgr[1], 27568, 300);
    EXPECT_NEAR(bgr[2], 49539, 300);
}

TEST(FfmpegVideo, ReportsAFileItCannotOpen) {
    const ScratchFolder folder;
    std::string error;

    const std::unique_ptr<FfmpegVideo> video = FfmpegVideo::open(folder.file("missing.mov"), false, error);

    EXPECT_EQ(video, nullptr);
    EXPECT_FALSE(error.empty());
}

#if defined(__APPLE__)
// VideoToolbox decodes HEVC, so the clip is HEVC when this Mac can encode it (a virtual machine may not);
// otherwise the hardware is tried on a codec it has no decoder for and software takes over. Either way the
// frames match software decoding's.
TEST(FfmpegVideo, HardwareDecodingOnMacOS) {
    const ScratchFolder folder;
    std::string path = folder.file("clip.mov");
    const Clip hevc{.encoder = "hevc_videotoolbox",
                    .format = AV_PIX_FMT_P010LE,
                    .size = {256, 144},
                    .matrix = AVCOL_SPC_BT2020_NCL,
                    .transfer = AVCOL_TRC_ARIB_STD_B67,
                    .primaries = AVCOL_PRI_BT2020,
                    .y = {64, 940, 1023, 500}};
    if (!writeClip(path, hevc)) {
        path = folder.file("clip.mkv");
        ASSERT_TRUE(writeClip(path, Clip{.y = {64, 940, 1023, 500}}));
    }
    std::string error;
    const std::unique_ptr<FfmpegVideo> hardware = FfmpegVideo::open(path, true, error);
    ASSERT_NE(hardware, nullptr) << error;
    const std::unique_ptr<FfmpegVideo> software = openInSoftware(path);
    ASSERT_NE(software, nullptr);

    const std::vector<VideoFrame> hardwareFrames = readAll(*hardware);
    const std::vector<VideoFrame> softwareFrames = readAll(*software);

    SCOPED_TRACE(path + " decoded by " + hardware->info().decoder);
    EXPECT_TRUE(hardware->info().decoder == "VideoToolbox" || hardware->info().decoder == "software");
    EXPECT_EQ(software->info().decoder, "software");
    EXPECT_EQ(hardware->info().valueRange, software->info().valueRange);
    ASSERT_EQ(hardwareFrames.size(), softwareFrames.size());
    ASSERT_EQ(hardwareFrames.size(), 4U);
    for (std::size_t i = 0; i < hardwareFrames.size(); ++i) {
        const cv::Vec3w fromHardware = middleOf(hardwareFrames[i]);
        const cv::Vec3w fromSoftware = middleOf(softwareFrames[i]);
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_NEAR(fromHardware[channel], fromSoftware[channel], 64)
                << "frame " << i << " channel " << channel;
        }
    }
}
#endif

} // namespace

// --- 8-bit SDR as 8-bit BGR, as the lab replays an ordinary video
// ----------------------------------------------------------------

namespace {

std::unique_ptr<FfmpegVideo> openEightBit(const std::string& path) {
    std::string error;
    std::unique_ptr<FfmpegVideo> video =
        FfmpegVideo::open(path, {.allowHardware = false, .eightBitAsEightBit = true}, error);
    EXPECT_NE(video, nullptr) << error;
    return video;
}

// The first frame's top-left pixel of an 8-bit clip of one colour, read as 8-bit BGR.
cv::Vec3b eightBitColourOf(const Clip& clip) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    EXPECT_TRUE(writeClip(path, clip));
    const std::unique_ptr<FfmpegVideo> video = openEightBit(path);
    if (!video) {
        return {};
    }
    const std::optional<VideoFrame> frame = video->read();
    if (!frame || frame->image.type() != CV_8UC3) {
        ADD_FAILURE() << "no 8-bit frame";
        return {};
    }
    return frame->image.at<cv::Vec3b>(0, 0);
}

Clip eightBit(int y, int u, int v, AVColorSpace matrix, AVColorRange range = AVCOL_RANGE_MPEG) {
    return Clip{
        .format = AV_PIX_FMT_YUV420P, .range = range, .matrix = matrix, .y = {y, y, y, y}, .u = u, .v = v};
}

void expectNear(const cv::Vec3b& pixel, const cv::Vec3b& expected, const char* what) {
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(pixel[channel], expected[channel], 3) << what << ", channel " << channel;
    }
}

TEST(FfmpegVideo, EightBitStaysEightBit) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, eightBit(126, 128, 128, AVCOL_SPC_BT709)));
    const std::unique_ptr<FfmpegVideo> video = openEightBit(path);
    ASSERT_NE(video, nullptr);

    const std::optional<VideoFrame> frame = video->read();

    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->image.type(), CV_8UC3);
    EXPECT_EQ(video->info().bitDepth, 8);
    EXPECT_TRUE(video->info().valueRange.empty());
}

// Red in each matrix, white and black at video range, and white kept where full range puts it.
TEST(FfmpegVideo, EightBitColoursFollowTheMatrixAndRange) {
    expectNear(eightBitColourOf(eightBit(81, 90, 240, AVCOL_SPC_SMPTE170M)), {0, 0, 254}, "BT.601 red");
    expectNear(eightBitColourOf(eightBit(63, 102, 240, AVCOL_SPC_BT709)), {0, 1, 255}, "BT.709 red");
    expectNear(eightBitColourOf(eightBit(235, 128, 128, AVCOL_SPC_BT709)), {255, 255, 255}, "video white");
    expectNear(eightBitColourOf(eightBit(16, 128, 128, AVCOL_SPC_BT709)), {0, 0, 0}, "video black");
    expectNear(eightBitColourOf(eightBit(235, 128, 128, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG)), {235, 235, 235},
               "full-range 235");
}

// An untagged video below 720 rows is standard definition: BT.601 (BT.709 would give green 24 here).
TEST(FfmpegVideo, UnknownMatrixBySize) {
    expectNear(eightBitColourOf(eightBit(81, 90, 240, AVCOL_SPC_UNSPECIFIED)), {0, 0, 254}, "untagged 64×48");
}

// An 8-bit HLG video keeps the 16-bit path, whose codes above white survive.
TEST(FfmpegVideo, EightBitHlgKeepsSixteenBits) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    Clip clip = eightBit(126, 128, 128, AVCOL_SPC_BT2020_NCL);
    clip.transfer = AVCOL_TRC_ARIB_STD_B67;
    ASSERT_TRUE(writeClip(path, clip));
    const std::unique_ptr<FfmpegVideo> video = openEightBit(path);
    ASSERT_NE(video, nullptr);

    const std::optional<VideoFrame> frame = video->read();

    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->image.type(), CV_16UC3);
}

TEST(FfmpegVideo, SeekableWhenTimestamped) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, Clip{}));

    EXPECT_TRUE(openInSoftware(path)->info().seekable);
}

} // namespace
