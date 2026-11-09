#include "ffmpeg_clip.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace test_support {
namespace {

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

// What a clip is written with, whatever its frames hold.
struct Settings {
    const char* encoder;
    AVPixelFormat format;
    cv::Size size;
    AVColorRange range = AVCOL_RANGE_MPEG;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    std::optional<double> rotation;
    int fps = 30;
    int gop = 12;
    int bFrames = 0;
};

Written writeFrames(const std::string& path, const Settings& settings, int frames,
                    const std::function<void(AVFrame*, int)>& fillFrame) {
    av_log_set_level(AV_LOG_ERROR); // not FFV1's and MOV's notes on what they chose
    const AVCodec* codec = avcodec_find_encoder_by_name(settings.encoder);
    if (!codec) {
        return Written::NoEncoder;
    }
    AVFormatContext* rawOutput = nullptr;
    if (avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, path.c_str()) < 0) {
        return Written::Failed;
    }
    const std::unique_ptr<AVFormatContext, CloseOutput> output(rawOutput);
    const std::unique_ptr<AVCodecContext, FreeEncoder> encoder(avcodec_alloc_context3(codec));
    if (!encoder) {
        return Written::Failed;
    }
    encoder->width = settings.size.width;
    encoder->height = settings.size.height;
    encoder->pix_fmt = settings.format;
    encoder->time_base = {1, settings.fps};
    encoder->framerate = {settings.fps, 1};
    encoder->gop_size = settings.gop;
    encoder->max_b_frames = settings.bFrames;
    encoder->color_range = settings.range;
    encoder->colorspace = settings.matrix;
    encoder->color_trc = settings.transfer;
    encoder->color_primaries = settings.primaries;
    if (output->oformat->flags & AVFMT_GLOBALHEADER) {
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    if (!stream || avcodec_open2(encoder.get(), codec, nullptr) < 0 ||
        avcodec_parameters_from_context(stream->codecpar, encoder.get()) < 0) {
        return Written::Failed;
    }
    stream->time_base = encoder->time_base;
    if (settings.rotation) {
        AVPacketSideData* matrix =
            av_packet_side_data_new(&stream->codecpar->coded_side_data, &stream->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t), 0);
        if (!matrix) {
            return Written::Failed;
        }
        av_display_rotation_set(reinterpret_cast<std::int32_t*>(matrix->data), *settings.rotation);
    }
    if (avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(output.get(), nullptr) < 0) {
        return Written::Failed;
    }

    const std::unique_ptr<AVFrame, FreeFrame> frame(av_frame_alloc());
    const std::unique_ptr<AVPacket, FreePacket> packet(av_packet_alloc());
    if (!frame || !packet) {
        return Written::Failed;
    }
    frame->format = settings.format;
    frame->width = settings.size.width;
    frame->height = settings.size.height;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        return Written::Failed;
    }
    for (int index = 0; index < frames; ++index) {
        if (av_frame_make_writable(frame.get()) < 0) {
            return Written::Failed;
        }
        fillFrame(frame.get(), index);
        frame->pts = index;
        if (!encode(encoder.get(), frame.get(), output.get(), stream, packet.get())) {
            return Written::Failed;
        }
    }
    return encode(encoder.get(), nullptr, output.get(), stream, packet.get()) &&
                   av_write_trailer(output.get()) == 0
               ? Written::Yes
               : Written::Failed;
}

} // namespace

bool writeClip(const std::string& path, const Clip& clip) {
    const Settings settings{.encoder = clip.encoder,
                            .format = clip.format,
                            .size = clip.size,
                            .range = clip.range,
                            .matrix = clip.matrix,
                            .transfer = clip.transfer,
                            .primaries = clip.primaries,
                            .rotation = clip.rotation};
    return writeFrames(path, settings, kClipFrames,
                       [&clip](AVFrame* frame, int index) { fill(frame, clip, index); }) == Written::Yes;
}

Written writeNumberedClip(const std::string& path, const NumberedClip& clip) {
    const int shift = av_pix_fmt_desc_get(clip.format)->comp[0].depth - 8;
    Clip frameClip{.encoder = clip.encoder, .format = clip.format, .size = clip.size};
    frameClip.u = 128 << shift;
    frameClip.v = 128 << shift;
    const Settings settings{.encoder = clip.encoder,
                            .format = clip.format,
                            .size = clip.size,
                            .fps = clip.fps,
                            .gop = clip.gop,
                            .bFrames = clip.bFrames};
    return writeFrames(path, settings, clip.frames, [&frameClip, shift](AVFrame* frame, int index) {
        frameClip.y.fill((16 + 3 * index) << shift);
        fill(frame, frameClip, 0);
    });
}

} // namespace test_support
