#include "frame_io/ffmpeg_video.h"

#include <opencv2/core.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace frame_io {
namespace {

using visual_debug::Primaries;
using visual_debug::Transfer;

constexpr int kUnity = 1 << 16; // 1.0 in swscale's 16.16 fixed point
constexpr double kFullScale = 65535.0;

#if defined(__APPLE__)
constexpr AVHWDeviceType kHardware = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
constexpr const char* kHardwareName = "VideoToolbox";
#elif defined(_WIN32)
constexpr AVHWDeviceType kHardware = AV_HWDEVICE_TYPE_D3D11VA;
constexpr const char* kHardwareName = "D3D11VA";
#else
constexpr AVHWDeviceType kHardware = AV_HWDEVICE_TYPE_NONE;
constexpr const char* kHardwareName = "";
#endif

struct CloseInput {
    void operator()(AVFormatContext* input) const { avformat_close_input(&input); }
};
struct FreeDecoder {
    void operator()(AVCodecContext* decoder) const { avcodec_free_context(&decoder); }
};
struct FreeFrame {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct FreePacket {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct FreeScaler {
    void operator()(SwsContext* scaler) const { sws_freeContext(scaler); }
};

using Input = std::unique_ptr<AVFormatContext, CloseInput>;
using Decoder = std::unique_ptr<AVCodecContext, FreeDecoder>;
using Frame = std::unique_ptr<AVFrame, FreeFrame>;
using Packet = std::unique_ptr<AVPacket, FreePacket>;
using Scaler = std::unique_ptr<SwsContext, FreeScaler>;

std::string describe(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return text.data();
}

// The format in which `codec` hands over frames that kHardware decoded, or none.
AVPixelFormat hardwareFormatOf(const AVCodec* codec) {
    if constexpr (kHardware == AV_HWDEVICE_TYPE_NONE) {
        return AV_PIX_FMT_NONE;
    }
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            return AV_PIX_FMT_NONE;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == kHardware) {
            return config->pix_fmt;
        }
    }
}

// The hardware's format when the decoder offers it for this stream; otherwise the decoder's own choice, which
// decodes in software when the hardware cannot take the stream.
AVPixelFormat chooseFormat(AVCodecContext* decoder, const AVPixelFormat* offered) {
    const AVPixelFormat wanted = *static_cast<const AVPixelFormat*>(decoder->opaque);
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == wanted) {
            return wanted;
        }
    }
    return avcodec_default_get_format(decoder, offered);
}

// Clockwise quarter turns, decided as FFmpeg's own player decides them from the stream's display matrix.
int quarterTurnsOf(const AVCodecParameters* parameters) {
    const AVPacketSideData* matrix = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!matrix || matrix->size < 9 * sizeof(std::int32_t)) {
        return 0;
    }
    const double rotation = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix->data));
    if (std::isnan(rotation)) {
        return 0;
    }
    double degrees = -std::round(rotation);
    degrees -= 360.0 * std::floor(degrees / 360.0 + 0.9 / 360.0); // 0 to 359
    if (degrees == 90.0) {
        return 1;
    }
    if (degrees == 180.0) {
        return 2;
    }
    return degrees == 270.0 ? 3 : 0;
}

int dolbyVisionProfileOf(const AVCodecParameters* parameters) {
    const AVPacketSideData* record = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (!record || record->size < sizeof(AVDOVIDecoderConfigurationRecord)) {
        return 0;
    }
    return reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(record->data)->dv_profile;
}

// The frame count the header gives: the stream's own (MOV and MP4 store it), else its duration, or the
// file's, at its average rate (Matroska stores only a duration). Nothing if the header gives neither.
std::optional<std::uint64_t> frameCountOf(const AVFormatContext& input, const AVStream& stream) {
    if (stream.nb_frames > 0) {
        return static_cast<std::uint64_t>(stream.nb_frames);
    }
    const AVRational rate = stream.avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        return std::nullopt;
    }
    double seconds = 0.0;
    if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0) {
        seconds = static_cast<double>(stream.duration) * av_q2d(stream.time_base);
    } else if (input.duration != AV_NOPTS_VALUE && input.duration > 0) {
        seconds = static_cast<double>(input.duration) / static_cast<double>(AV_TIME_BASE);
    } else {
        return std::nullopt;
    }
    const double frames = std::round(seconds * av_q2d(rate));
    return frames >= 1.0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(frames)) : std::nullopt;
}

// swscale numbers its matrices as FFmpeg's colour spaces, apart from the unknown ones, which are BT.709 here.
int swsColourspaceOf(AVColorSpace matrix) {
    switch (matrix) {
    case AVCOL_SPC_RGB:
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_RESERVED:
        return SWS_CS_ITU709;
    default:
        return static_cast<int>(matrix);
    }
}

// How swscale turns one source's YUV into 16-bit BGR. Every video converts as if full range, so each luma
// code keeps its place on the full scale: a limited-range video's codes above nominal white keep their values
// instead of clipping, its black (16 · 2^(b−8) of 2^b − 1) and white (235 · 2^(b−8)) landing where those
// codes are. Its chroma spans 224 codes to luma's 219, hence the saturation. (Contrast and brightness can
// do the same for 10 bits, but swscale's 8-bit path applies brightness differently: 16 of 255 came out at 0.)
struct Conversion {
    int colourspace = SWS_CS_ITU709;
    int saturation = kUnity;
    int contrast = kUnity;
};

Scaler makeScaler(AVPixelFormat format, cv::Size size, const Conversion& conversion) {
    Scaler scaler(sws_alloc_context());
    if (!scaler) {
        return nullptr;
    }
    // Only sws_scale_frame uses the threads (a 4K frame converts about seven times faster with 14 of them).
    const auto threads = static_cast<std::int64_t>(std::max(1U, std::thread::hardware_concurrency()));
    SwsContext* context = scaler.get();
    if (av_opt_set_int(context, "srcw", size.width, 0) < 0 ||
        av_opt_set_int(context, "srch", size.height, 0) < 0 ||
        av_opt_set_int(context, "src_format", format, 0) < 0 ||
        av_opt_set_int(context, "dstw", size.width, 0) < 0 ||
        av_opt_set_int(context, "dsth", size.height, 0) < 0 ||
        av_opt_set_int(context, "dst_format", AV_PIX_FMT_BGR48, 0) < 0 ||
        av_opt_set_int(context, "threads", threads, 0) < 0 ||
        sws_init_context(context, nullptr, nullptr) < 0) {
        return nullptr;
    }
    if (sws_setColorspaceDetails(context, sws_getCoefficients(conversion.colourspace), 1,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, conversion.contrast,
                                 conversion.saturation) < 0) {
        return nullptr;
    }
    return scaler;
}

// The ordinary conversion of an 8-bit video to 8-bit BGR: video range stretched to full (16 to 0, 235 to
// 255), full range kept.
Scaler makeEightBitScaler(AVPixelFormat format, cv::Size size, int colourspace, bool fullRange) {
    Scaler scaler(sws_alloc_context());
    if (!scaler) {
        return nullptr;
    }
    const auto threads = static_cast<std::int64_t>(std::max(1U, std::thread::hardware_concurrency()));
    SwsContext* context = scaler.get();
    if (av_opt_set_int(context, "srcw", size.width, 0) < 0 ||
        av_opt_set_int(context, "srch", size.height, 0) < 0 ||
        av_opt_set_int(context, "src_format", format, 0) < 0 ||
        av_opt_set_int(context, "dstw", size.width, 0) < 0 ||
        av_opt_set_int(context, "dsth", size.height, 0) < 0 ||
        av_opt_set_int(context, "dst_format", AV_PIX_FMT_BGR24, 0) < 0 ||
        av_opt_set_int(context, "threads", threads, 0) < 0 ||
        sws_init_context(context, nullptr, nullptr) < 0) {
        return nullptr;
    }
    const int* coefficients = sws_getCoefficients(colourspace);
    if (sws_setColorspaceDetails(context, coefficients, fullRange ? 1 : 0, coefficients, 1, 0, kUnity,
                                 kUnity) < 0) {
        return nullptr;
    }
    return scaler;
}

// The full-range ("JPEG") formats FFmpeg still uses for MJPEG, whatever the range tag says.
bool isFullRangeFormat(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
        return true;
    default:
        return false;
    }
}

// Converts `source` into `target`, a matrix of its size in `targetFormat` (BGR48: CV_16UC3, BGR24: CV_8UC3).
bool convert(SwsContext* scaler, const AVFrame* source, cv::Mat& target,
             AVPixelFormat targetFormat = AV_PIX_FMT_BGR48) {
    const Frame frame(av_frame_alloc());
    if (!frame) {
        return false;
    }
    frame->format = targetFormat;
    frame->width = target.cols;
    frame->height = target.rows;
    frame->data[0] = target.data;
    frame->linesize[0] = static_cast<int>(target.step);
    // Without a buffer of its own the frame would get one from swscale; this one wraps the matrix's memory,
    // which the matrix owns, so freeing it does nothing.
    frame->buf[0] = av_buffer_create(
        target.data, target.step * static_cast<std::size_t>(target.rows), [](void*, std::uint8_t*) {},
        nullptr, 0);
    return frame->buf[0] && sws_scale_frame(scaler, frame.get(), source) >= 0;
}

int planeSize(int size, int log2Subsampling) {
    return (size + (1 << log2Subsampling) - 1) >> log2Subsampling;
}

// Converts a small frame in `format` holding three bands of luma `codes` with neutral chroma; what each band
// came out as.
std::optional<std::array<double, 3>> convertBands(AVPixelFormat format, const Conversion& conversion,
                                                  const std::array<int, 3>& codes) {
    constexpr int kBand = 16;
    const cv::Size size(3 * kBand, kBand);
    const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(format);
    const Frame sample(av_frame_alloc());
    if (!description || !sample) {
        return std::nullopt;
    }
    sample->format = format;
    sample->width = size.width;
    sample->height = size.height;
    if (av_frame_get_buffer(sample.get(), 0) < 0) {
        return std::nullopt;
    }
    const bool rgb = (description->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    // av_write_image_line2 adds its bits to those already there.
    for (int plane = 0; plane < 4 && sample->data[plane]; ++plane) {
        const int height =
            planeSize(size.height, !rgb && (plane == 1 || plane == 2) ? description->log2_chroma_h : 0);
        std::memset(sample->data[plane], 0, static_cast<std::size_t>(sample->linesize[plane]) * height);
    }
    std::vector<std::uint16_t> row(static_cast<std::size_t>(size.width));
    for (int component = 0; component < description->nb_components; ++component) {
        const bool chroma = !rgb && (component == 1 || component == 2);
        const int log2Width = chroma ? description->log2_chroma_w : 0;
        const int width = planeSize(size.width, log2Width);
        const int height = planeSize(size.height, chroma ? description->log2_chroma_h : 0);
        const int depth = description->comp[component].depth;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int code = codes[static_cast<std::size_t>((x << log2Width) / kBand)];
                const int value = component == 3 ? (1 << depth) - 1 : chroma ? 1 << (depth - 1) : code;
                row[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(value);
            }
            av_write_image_line2(row.data(), sample->data, sample->linesize, description, 0, y, component,
                                 width, sizeof(std::uint16_t));
        }
    }
    const Scaler scaler = makeScaler(format, size, conversion);
    cv::Mat converted(size, CV_16UC3);
    if (!scaler || !convert(scaler.get(), sample.get(), converted)) {
        return std::nullopt;
    }
    std::array<double, 3> levels{};
    for (std::size_t band = 0; band < levels.size(); ++band) {
        levels[band] = converted.at<cv::Vec3w>(kBand / 2, static_cast<int>(band) * kBand + kBand / 2)[1];
    }
    return levels;
}

struct Calibration {
    Conversion conversion;
    visual_debug::ValueRange valueRange; // black and white, as converted
};

// The conversion for frames in `format`, checked on black, nominal white and the top code.
std::optional<Calibration> calibrate(AVPixelFormat format, AVColorSpace matrix, AVColorRange range) {
    const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(format);
    if (!description) {
        return std::nullopt;
    }
    const int depth = description->comp[0].depth;
    const int top = (1 << depth) - 1;
    Conversion conversion{.colourspace = swsColourspaceOf(matrix)};
    std::array<int, 3> codes{0, top, top};
    if (range != AVCOL_RANGE_JPEG && (description->flags & AV_PIX_FMT_FLAG_RGB) == 0) {
        const double step = std::ldexp(1.0, depth - 8);
        conversion.saturation = 219 * kUnity / 224;
        codes = {static_cast<int>(16.0 * step), static_cast<int>(235.0 * step), top};
    }
    // FFmpeg's 16-bit output from 10 bits runs short of the full scale (1023 of 1023 gave 65472). Contrast
    // multiplies every level, so raising it puts the top code at 65535, and black and white at their places
    // with it.
    std::optional<std::array<double, 3>> levels = convertBands(format, conversion, codes);
    for (int attempt = 0; attempt < 4 && levels && (*levels)[2] > 0.0 && (*levels)[2] < kFullScale;
         ++attempt) {
        const auto raised = static_cast<int>(std::ceil(conversion.contrast * kFullScale / (*levels)[2]));
        conversion.contrast = std::max(raised, conversion.contrast + 1);
        levels = convertBands(format, conversion, codes);
    }
    if (!levels) {
        return std::nullopt;
    }
    return Calibration{conversion, {(*levels)[0], (*levels)[1]}};
}

Transfer transferOf(AVColorTransferCharacteristic transfer) {
    switch (transfer) {
    case AVCOL_TRC_ARIB_STD_B67:
        return Transfer::Hlg;
    case AVCOL_TRC_SMPTE2084:
        return Transfer::Pq;
    default:
        return Transfer::Sdr;
    }
}

// The frame's own tag, which the decoder takes from the bitstream, or the container's.
template <typename T> T tagOf(T fromFrame, T fromContainer, T unknown) {
    return fromFrame != unknown ? fromFrame : fromContainer;
}

} // namespace

struct FfmpegVideo::Impl {
    Input input;
    Decoder decoder;
    int stream = -1;
    AVRational timeBase{};
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE; // the decoder's opaque points here
    Packet packet{av_packet_alloc()};
    Frame decoded{av_frame_alloc()};
    Frame transferred{av_frame_alloc()}; // from the hardware's memory
    bool draining = false;
    Conversion conversion;
    // An 8-bit SDR video read as 8-bit: its own conversion instead of the 16-bit one.
    bool eightBitOutput = false;
    int eightBitColourspace = SWS_CS_ITU709;
    bool eightBitFullRange = false;
    Scaler scaler;
    AVPixelFormat scalerFormat = AV_PIX_FMT_NONE;
    cv::Size scalerSize;
    int quarterTurns = 0;
    std::optional<std::int64_t> firstTimestamp;
    std::int64_t framesRead = 0;
    VideoInfo info;
    std::optional<VideoFrame> first; // decoded at open, to learn the frames' format
    std::string error;               // why the last read gave nothing; empty at the end

    // One attempt at opening `path`: with the hardware when `hardware` is set and the codec has it.
    static std::unique_ptr<Impl> open(const std::string& path, bool hardware, bool eightBitAsEightBit,
                                      std::string& error);
    const AVFrame* nextFrame();
    const AVFrame* receiveFrame();
    const AVFrame* inMemory(const AVFrame* frame);
    bool seek(std::chrono::nanoseconds at, const std::function<bool()>& abandon);
    std::optional<VideoFrame> toVideoFrame(const AVFrame* frame);
    std::nullptr_t fail(const char* what, int code); // sets `error`
};

std::nullptr_t FfmpegVideo::Impl::fail(const char* what, int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    error = std::string(what) + ": " + text.data();
    return nullptr;
}

// The next decoded frame in the computer's memory; nothing at the end or on an error, which sets `error`.
const AVFrame* FfmpegVideo::Impl::nextFrame() {
    const AVFrame* frame = receiveFrame();
    return frame ? inMemory(frame) : nullptr;
}

// The next decoded frame, in the hardware's memory when the hardware decodes; nothing at the end or on an
// error, which sets `error`.
const AVFrame* FfmpegVideo::Impl::receiveFrame() {
    for (;;) {
        const int received = avcodec_receive_frame(decoder.get(), decoded.get());
        if (received == 0) {
            return decoded.get();
        }
        if (received == AVERROR_EOF) {
            return nullptr; // the end: the decoder has handed over every frame it held
        }
        if (received != AVERROR(EAGAIN)) {
            return fail("could not decode a frame", received);
        }
        if (draining) {
            return nullptr; // nothing more will come
        }
        if (const int read = av_read_frame(input.get(), packet.get()); read < 0) {
            if (read != AVERROR_EOF) {
                return fail("could not read the file", read);
            }
            draining = true; // the end of the file: the decoder hands over the frames it still holds
            if (const int sent = avcodec_send_packet(decoder.get(), nullptr); sent < 0) {
                return fail("could not finish decoding", sent);
            }
            continue;
        }
        const int sent =
            packet->stream_index == stream ? avcodec_send_packet(decoder.get(), packet.get()) : 0;
        av_packet_unref(packet.get());
        if (sent < 0) {
            return fail("could not decode a packet", sent);
        }
    }
}

// The decoded frame in the computer's memory: itself, or copied from the hardware's.
const AVFrame* FfmpegVideo::Impl::inMemory(const AVFrame* frame) {
    if (!frame->hw_frames_ctx) {
        return frame;
    }
    av_frame_unref(transferred.get());
    if (const int transferredData = av_hwframe_transfer_data(transferred.get(), frame, 0);
        transferredData < 0) {
        return fail("could not copy a frame from the hardware", transferredData);
    }
    if (const int copied = av_frame_copy_props(transferred.get(), frame); copied < 0) {
        return fail("could not copy a frame from the hardware", copied);
    }
    return transferred.get();
}

bool FfmpegVideo::Impl::seek(std::chrono::nanoseconds at, const std::function<bool()>& abandon) {
    error.clear();
    first.reset();
    if (!firstTimestamp) {
        error = "its frames have no timestamps";
        return false;
    }
    const AVRational nanoseconds{1, 1'000'000'000};
    const std::int64_t target = *firstTimestamp + av_rescale_q(at.count(), nanoseconds, timeBase);
    // Half a frame, compared in nanoseconds: in a time base of one tick a frame (AVI, IVF), half a frame
    // rounded to ticks would be a whole one, and the frame before the target would pass for it.
    const std::int64_t half = info.fps > 0.0 ? std::llround(0.5e9 / info.fps) : 0;
    const auto fromTarget = [&](std::int64_t timestamp) {
        return av_rescale_q(timestamp - *firstTimestamp, timeBase, nanoseconds) - at.count();
    };
    std::int64_t from = target;
    std::int64_t back = av_rescale_q(1'000'000'000, nanoseconds, timeBase); // a second, doubled each try
    for (;;) {
        if (const int sought = av_seek_frame(input.get(), stream, from, AVSEEK_FLAG_BACKWARD); sought < 0) {
            fail("could not seek", sought);
            return false;
        }
        avcodec_flush_buffers(decoder.get());
        draining = false;
        bool firstDecoded = true;
        bool tryEarlier = false;
        while (!tryEarlier) {
            if (abandon && abandon()) {
                error.clear();
                return false;
            }
            const AVFrame* frame = receiveFrame();
            if (!frame) {
                return error.empty(); // past the end: the next read gives the end
            }
            // Compared before any copy or conversion: the frames passed over cost only their decoding.
            const std::int64_t timestamp = frame->best_effort_timestamp;
            // The first frame decoded already past the target: the seek landed late (an index by decoding
            // time, an open GOP's dropped leading frames), so try again from earlier, down to the first
            // frame.
            if (firstDecoded && timestamp != AV_NOPTS_VALUE && fromTarget(timestamp) > half &&
                from > *firstTimestamp) {
                from = std::max(target - back, *firstTimestamp);
                back *= 2;
                tryEarlier = true;
                continue;
            }
            firstDecoded = false;
            if (timestamp == AV_NOPTS_VALUE || fromTarget(timestamp) >= -half) {
                const AVFrame* kept = inMemory(frame);
                if (!kept) {
                    return false;
                }
                first = toVideoFrame(kept);
                if (!first) {
                    error = "could not convert a frame to BGR";
                    return false;
                }
                return true;
            }
        }
    }
}

std::optional<VideoFrame> FfmpegVideo::Impl::toVideoFrame(const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const cv::Size size(frame->width, frame->height);
    if (!scaler || format != scalerFormat || size != scalerSize) {
        scaler = eightBitOutput ? makeEightBitScaler(format, size, eightBitColourspace,
                                                     eightBitFullRange || isFullRangeFormat(format))
                                : makeScaler(format, size, conversion);
        scalerFormat = format;
        scalerSize = size;
        if (!scaler) {
            return std::nullopt;
        }
    }
    cv::Mat image(size, eightBitOutput ? CV_8UC3 : CV_16UC3);
    if (!convert(scaler.get(), frame, image, eightBitOutput ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_BGR48)) {
        return std::nullopt;
    }
    if (quarterTurns != 0) {
        static constexpr std::array kTurns{cv::ROTATE_90_CLOCKWISE, cv::ROTATE_180,
                                           cv::ROTATE_90_COUNTERCLOCKWISE};
        cv::Mat upright;
        cv::rotate(image, upright, kTurns[static_cast<std::size_t>(quarterTurns - 1)]);
        image = upright;
    }

    std::chrono::nanoseconds at{};
    const std::int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp != AV_NOPTS_VALUE) {
        if (!firstTimestamp) {
            firstTimestamp = timestamp;
        }
        at = std::chrono::nanoseconds(
            av_rescale_q(timestamp - *firstTimestamp, timeBase, AVRational{1, 1'000'000'000}));
    } else if (info.fps > 0.0) {
        at = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(static_cast<double>(framesRead) / info.fps));
    }
    ++framesRead;
    return VideoFrame{image, at};
}

std::unique_ptr<FfmpegVideo::Impl> FfmpegVideo::Impl::open(const std::string& path, bool hardware,
                                                           bool eightBitAsEightBit, std::string& error) {
    auto impl = std::make_unique<Impl>();
    if (!impl->packet || !impl->decoded || !impl->transferred) {
        error = "out of memory";
        return nullptr;
    }
    AVFormatContext* input = nullptr;
    if (const int opened = avformat_open_input(&input, path.c_str(), nullptr, nullptr); opened < 0) {
        error = describe(opened);
        return nullptr;
    }
    impl->input.reset(input);
    if (const int found = avformat_find_stream_info(input, nullptr); found < 0) {
        error = describe(found);
        return nullptr;
    }
    const AVCodec* codec = nullptr;
    impl->stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (impl->stream < 0) {
        error = impl->stream == AVERROR_DECODER_NOT_FOUND ? "FFmpeg has no decoder for its video"
                                                          : "no video in it";
        return nullptr;
    }
    const AVStream* stream = input->streams[impl->stream];
    const AVCodecParameters* parameters = stream->codecpar;
    impl->decoder.reset(avcodec_alloc_context3(codec));
    if (!impl->decoder || avcodec_parameters_to_context(impl->decoder.get(), parameters) < 0) {
        error = "cannot set up its decoder";
        return nullptr;
    }
    impl->decoder->pkt_timebase = stream->time_base;
    impl->timeBase = stream->time_base;

    impl->hardwareFormat =
        hardware && !codecPrefersSoftware(codec->name) ? hardwareFormatOf(codec) : AV_PIX_FMT_NONE;
    AVBufferRef* device = nullptr;
    if (impl->hardwareFormat != AV_PIX_FMT_NONE &&
        av_hwdevice_ctx_create(&device, kHardware, nullptr, nullptr, 0) >= 0) {
        impl->decoder->hw_device_ctx = device; // the decoder owns this reference
        impl->decoder->opaque = &impl->hardwareFormat;
        impl->decoder->get_format = chooseFormat;
    } else {
        impl->hardwareFormat = AV_PIX_FMT_NONE;
        impl->decoder->thread_count = 0; // as many as the computer has
    }
    if (const int opened = avcodec_open2(impl->decoder.get(), codec, nullptr); opened < 0) {
        error = describe(opened);
        return nullptr;
    }

    // The first frame shows the format the frames come in, which the conversion is calibrated for, and
    // whether the hardware decodes them.
    const AVFrame* first = impl->nextFrame();
    if (!first) {
        error = "no frame of it could be decoded";
        return nullptr;
    }
    const auto format = static_cast<AVPixelFormat>(first->format);
    const std::optional<Calibration> calibration =
        calibrate(format, tagOf(first->colorspace, parameters->color_space, AVCOL_SPC_UNSPECIFIED),
                  tagOf(first->color_range, parameters->color_range, AVCOL_RANGE_UNSPECIFIED));
    if (!calibration) {
        error = std::string("cannot convert its frames from ") + av_get_pix_fmt_name(format);
        return nullptr;
    }
    impl->conversion = calibration->conversion;
    impl->quarterTurns = quarterTurnsOf(parameters);

    VideoInfo& info = impl->info;
    info.size = impl->quarterTurns % 2 == 0 ? cv::Size(first->width, first->height)
                                            : cv::Size(first->height, first->width);
    // The average rate, else the stream's base rate (some containers give only that).
    const auto rateOf = [](AVRational rate) { return rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0; };
    info.fps =
        rateOf(stream->avg_frame_rate) > 0.0 ? rateOf(stream->avg_frame_rate) : rateOf(stream->r_frame_rate);
    info.frameCount = frameCountOf(*input, *stream);
    info.bitDepth = av_pix_fmt_desc_get(format)->comp[0].depth;
    info.valueRange = calibration->valueRange;
    info.transfer = transferOf(tagOf(first->color_trc, parameters->color_trc, AVCOL_TRC_UNSPECIFIED));
    info.primaries =
        tagOf(first->color_primaries, parameters->color_primaries, AVCOL_PRI_UNSPECIFIED) == AVCOL_PRI_BT2020
            ? Primaries::Bt2020
            : Primaries::Bt709;
    info.decoder = impl->decoded->hw_frames_ctx ? kHardwareName : "software";
    info.dolbyVisionProfile = dolbyVisionProfileOf(parameters);
    info.seekable = first->best_effort_timestamp != AV_NOPTS_VALUE && info.fps > 0.0 &&
                    (input->iformat->flags & AVFMT_NOTIMESTAMPS) == 0;

    // An 8-bit SDR video stays 8-bit when asked: its own matrix (an untagged one by its size, as players
    // guess: BT.601 below 720 rows) and range; no value range, as 0–255 is its whole scale.
    if (eightBitAsEightBit && info.bitDepth <= 8 && info.transfer == Transfer::Sdr) {
        const AVColorSpace matrix = tagOf(first->colorspace, parameters->color_space, AVCOL_SPC_UNSPECIFIED);
        impl->eightBitOutput = true;
        impl->eightBitColourspace = matrix == AVCOL_SPC_UNSPECIFIED || matrix == AVCOL_SPC_RESERVED
                                        ? (first->height < 720 ? SWS_CS_ITU601 : SWS_CS_ITU709)
                                        : swsColourspaceOf(matrix);
        impl->eightBitFullRange =
            tagOf(first->color_range, parameters->color_range, AVCOL_RANGE_UNSPECIFIED) == AVCOL_RANGE_JPEG;
        info.valueRange = {};
    }

    impl->first = impl->toVideoFrame(first);
    if (!impl->first) {
        error = "cannot convert its first frame";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<FfmpegVideo> FfmpegVideo::open(const std::string& path, const Options& options,
                                               std::string& error) {
    std::unique_ptr<Impl> impl =
        options.allowHardware ? Impl::open(path, true, options.eightBitAsEightBit, error) : nullptr;
    if (!impl) {
        impl = Impl::open(path, false, options.eightBitAsEightBit, error); // the hardware could not decode it
    }
    if (!impl) {
        return nullptr;
    }
    return std::unique_ptr<FfmpegVideo>(new FfmpegVideo(std::move(impl)));
}

std::unique_ptr<FfmpegVideo> FfmpegVideo::open(const std::string& path, bool allowHardware,
                                               std::string& error) {
    return open(path, Options{.allowHardware = allowHardware, .eightBitAsEightBit = false}, error);
}

// H.264 at 4K on Apple silicon: FFmpeg's own decoder read a 60 fps clip about four times as fast as
// VideoToolbox through FFmpeg did, and sought in well under half the time (A2.1's measurements). HEVC went
// the other way, so it keeps the hardware.
bool codecPrefersSoftware(std::string_view decoderName) { return decoderName == "h264"; }

FfmpegVideo::FfmpegVideo(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

FfmpegVideo::~FfmpegVideo() = default;

const VideoInfo& FfmpegVideo::info() const { return m_impl->info; }

std::optional<VideoFrame> FfmpegVideo::read() {
    m_impl->error.clear();
    if (m_impl->first) {
        return std::exchange(m_impl->first, std::nullopt);
    }
    const AVFrame* frame = m_impl->nextFrame();
    if (!frame) {
        return std::nullopt;
    }
    std::optional<VideoFrame> converted = m_impl->toVideoFrame(frame);
    if (!converted) {
        m_impl->error = "could not convert a frame to BGR";
    }
    return converted;
}

const std::string& FfmpegVideo::lastError() const { return m_impl->error; }

bool FfmpegVideo::seek(std::chrono::nanoseconds at, const std::function<bool()>& abandon) {
    return m_impl->seek(at, abandon);
}

} // namespace frame_io
