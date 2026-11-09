#pragma once

#include "visual_debug/image_message.h"
#include "visual_debug/overlay.h"

#include <opencv2/core/mat.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Wire format version 2: a two-frame ZeroMQ message holding a JSON header and the raw pixels.
// The authoritative description is docs/wire-format.md; change both together.
namespace visual_debug {

inline constexpr int kProtocolVersion = 2;
inline constexpr std::size_t kMaxNameBytes = 1024;
inline constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
inline constexpr std::size_t kMaxPixelBytes = std::size_t{512} * 1024 * 1024;
inline constexpr int kMaxDimension = 32768;
inline constexpr int kMaxChannels = 4;
// The end of the year 9999 in milliseconds since the Unix epoch: the largest sentAt a header may carry.
inline constexpr std::int64_t kMaxSentAtMilliseconds = 253402300799999;

// The optional header fields. A sentAt outside 0 to kMaxSentAtMilliseconds, and an empty value range, are
// left out.
struct EncodeOptions {
    bool live = false;
    std::optional<SentTime> sentAt{};
    ImageDescription description{};
};

struct EncodedMessage {
    std::string header;
    cv::Mat pixels; // always continuous; may share data with the image that was encoded

    std::span<const std::byte> pixelBytes() const;
};

// Reasons are fixed ASCII texts, never built from the input, so they are always safe to send back.
struct EncodeError {
    std::string reason;
};
struct DecodeError {
    std::string reason;
};

// True for a non-empty 2-D image of depth 8U to 64F with 1-4 channels, at most kMaxDimension on each
// side and kMaxPixelBytes in total. Reads only the image's metadata.
bool isSupportedImage(const cv::Mat& image);

// Replaces bytes that are not valid UTF-8 with U+FFFD, then cuts the name to kMaxNameBytes at a character
// boundary. encode applies it to every name; the viewer applies it to renamed images, so both follow one
// rule.
std::string sanitiseName(std::string_view name);

// The name goes through sanitiseName.
std::variant<EncodedMessage, EncodeError> encode(const cv::Mat& image, std::string_view name,
                                                 const EncodeOptions& options = {});

// Validates every header field and the pixel frame length before allocating. The returned image owns
// its pixels. Never throws.
std::variant<ImageMessage, DecodeError> decode(std::string_view header, std::span<const std::byte> pixels);

// The control messages, sent as a header and an empty frame: a ping asks whether a viewer is listening; an
// end tells it a live channel is finished with (docs/wire-format.md).
std::string encodePing();
// The name goes through sanitiseName.
std::string encodeEnd(std::string_view channel);

// What a header with an empty second frame asks for. Image: not a control message, to be decoded as an image.
struct ControlMessage {
    enum class Kind { Ping, End, Image };
    Kind kind = Kind::Ping;
    std::string name; // End's channel
};
// Reads only protocol, version, type and an end's name, with decode()'s reasons. Never throws, never copies
// the header.
std::variant<ControlMessage, DecodeError> decodeControl(std::string_view header);

// The overlays as the header's "overlays" array carries them: those the viewer would accept, at most
// kMaxOverlays, each number rounded to a thousandth; "[]" when none is left.
std::string encodeOverlays(const std::vector<Overlay>& overlays);

} // namespace visual_debug
