#include "ffmpeg_video.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

using live_sender::FfmpegVideo;
using live_sender::VideoFrame;
using visual_debug::Primaries;
using visual_debug::Transfer;

// A folder of its own under the system's temporary one, removed afterwards. Random, as test programs run in
// parallel.
class ScratchFolder {
public:
    ScratchFolder()
        : m_path(std::filesystem::temp_directory_path() /
                 ("live_sender_ffmpeg_" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(m_path);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ~ScratchFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    std::string file(const std::string& name) const { return (m_path / name).string(); }

private:
    std::filesystem::path m_path;
};

constexpr int kFrames = 4;

// A test clip: four frames at 30 fps, each of one Y, U and V, with an optional block of another Y, tagged as
// given. The encoder copies the tags to the stream, where Matroska and MOV store them.
struct Clip {
    const char* encoder = "ffv1"; // lossless, so the codes read back are the codes written
    AVPixelFormat format = AV_PIX_FMT_YUV420P10LE;
    cv::Size size{64, 48};
    AVColorRange range = AVCOL_RANGE_MPEG;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    std::array<int, kFrames> y{64, 64, 64, 64}; // each frame's
    int u = 512;
    int v = 512;
    std::optional<cv::Rect> block{}; // drawn with blockY
    int blockY = 940;
    std::optional<double> rotation{}; // degrees, as av_display_rotation_set takes them
};

struct FreeEncoder {
    void operator()(AVCodecContext* encoder) const { avcodec_free_context(&encoder); }
};
struct FreeFrame {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct FreePacket {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct CloseOutput {
    void operator()(AVFormatContext* output) const {
        if (output->pb) {
            avio_closep(&output->pb);
        }
        avformat_free_context(output);
    }
};

int planeSize(int size, int log2Subsampling) {
    return (size + (1 << log2Subsampling) - 1) >> log2Subsampling;
}

void fill(AVFrame* frame, const Clip& clip, int index) {
    const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(clip.format);
    // av_write_image_line2 adds its bits to those already there.
    for (int plane = 0; plane < 4 && frame->data[plane]; ++plane) {
        const int height = plane == 1 || plane == 2 ? planeSize(clip.size.height, description->log2_chroma_h)
                                                    : clip.size.height;
        std::memset(frame->data[plane], 0, static_cast<std::size_t>(frame->linesize[plane]) * height);
    }
    std::vector<std::uint16_t> row(static_cast<std::size_t>(clip.size.width));
    for (int component = 0; component < 3; ++component) {
        const int width =
            component == 0 ? clip.size.width : planeSize(clip.size.width, description->log2_chroma_w);
        const int height =
            component == 0 ? clip.size.height : planeSize(clip.size.height, description->log2_chroma_h);
        const int value = component == 0   ? clip.y[static_cast<std::size_t>(index)]
                          : component == 1 ? clip.u
                                           : clip.v;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool inBlock = component == 0 && clip.block && clip.block->contains(cv::Point(x, y));
                row[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(inBlock ? clip.blockY : value);
            }
            av_write_image_line2(row.data(), frame->data, frame->linesize, description, 0, y, component,
                                 width, sizeof(std::uint16_t));
        }
    }
}

// Sends `frame` (nothing to flush the encoder) and writes every packet the encoder has ready.
bool encode(AVCodecContext* encoder, const AVFrame* frame, AVFormatContext* output, const AVStream* stream,
            AVPacket* packet) {
    if (avcodec_send_frame(encoder, frame) < 0) {
        return false;
    }
    for (;;) {
        const int received = avcodec_receive_packet(encoder, packet);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return true;
        }
        if (received < 0) {
            return false;
        }
        // A frame each, as FFmpeg's own program says: VideoToolbox gives none, and MOV's edit list would then
        // end before the last frame.
        if (packet->duration == 0) {
            packet->duration = 1;
        }
        av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        if (av_interleaved_write_frame(output, packet) < 0) {
            return false;
        }
    }
}

// The container follows the file name's extension.
bool writeClip(const std::string& path, const Clip& clip) {
    av_log_set_level(AV_LOG_ERROR); // not FFV1's and MOV's notes on what they chose
    const AVCodec* codec = avcodec_find_encoder_by_name(clip.encoder);
    AVFormatContext* rawOutput = nullptr;
    if (!codec || avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, path.c_str()) < 0) {
        return false;
    }
    const std::unique_ptr<AVFormatContext, CloseOutput> output(rawOutput);
    const std::unique_ptr<AVCodecContext, FreeEncoder> encoder(avcodec_alloc_context3(codec));
    if (!encoder) {
        return false;
    }
    encoder->width = clip.size.width;
    encoder->height = clip.size.height;
    encoder->pix_fmt = clip.format;
    encoder->time_base = {1, 30};
    encoder->framerate = {30, 1};
    encoder->color_range = clip.range;
    encoder->colorspace = clip.matrix;
    encoder->color_trc = clip.transfer;
    encoder->color_primaries = clip.primaries;
    if (output->oformat->flags & AVFMT_GLOBALHEADER) {
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    if (!stream || avcodec_open2(encoder.get(), codec, nullptr) < 0 ||
        avcodec_parameters_from_context(stream->codecpar, encoder.get()) < 0) {
        return false;
    }
    stream->time_base = encoder->time_base;
    if (clip.rotation) {
        AVPacketSideData* matrix =
            av_packet_side_data_new(&stream->codecpar->coded_side_data, &stream->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t), 0);
        if (!matrix) {
            return false;
        }
        av_display_rotation_set(reinterpret_cast<std::int32_t*>(matrix->data), *clip.rotation);
    }
    if (avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(output.get(), nullptr) < 0) {
        return false;
    }

    const std::unique_ptr<AVFrame, FreeFrame> frame(av_frame_alloc());
    const std::unique_ptr<AVPacket, FreePacket> packet(av_packet_alloc());
    if (!frame || !packet) {
        return false;
    }
    frame->format = clip.format;
    frame->width = clip.size.width;
    frame->height = clip.size.height;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        return false;
    }
    for (int index = 0; index < kFrames; ++index) {
        if (av_frame_make_writable(frame.get()) < 0) {
            return false;
        }
        fill(frame.get(), clip, index);
        frame->pts = index;
        if (!encode(encoder.get(), frame.get(), output.get(), stream, packet.get())) {
            return false;
        }
    }
    return encode(encoder.get(), nullptr, output.get(), stream, packet.get()) &&
           av_write_trailer(output.get()) == 0;
}

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
