#include "visual_debug/wire_format.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace visual_debug {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kProtocolName = "visual_debug";
constexpr std::string_view kImageType = "image";

struct DepthInfo {
    int depth;
    std::string_view name;
    std::size_t elementSize;
};

// The protocol's own table, deliberately not derived from OpenCV: OpenCV 5 adds depths (16F, 16BF, Bool,
// 64U, 64S, 32U) that version 2 of the format does not carry, and the names are part of the wire contract.
constexpr std::array<DepthInfo, 7> kDepths{{
    {CV_8U, "8U", 1},
    {CV_8S, "8S", 1},
    {CV_16U, "16U", 2},
    {CV_16S, "16S", 2},
    {CV_32S, "32S", 4},
    {CV_32F, "32F", 4},
    {CV_64F, "64F", 8},
}};

// The names are part of the wire contract, like the depths'.
constexpr std::array<std::pair<Transfer, std::string_view>, 3> kTransfers{{
    {Transfer::Sdr, "sdr"},
    {Transfer::Hlg, "hlg"},
    {Transfer::Pq, "pq"},
}};
constexpr std::array<std::pair<Primaries, std::string_view>, 2> kPrimaries{{
    {Primaries::Bt709, "bt709"},
    {Primaries::Bt2020, "bt2020"},
}};

template <typename Enum, std::size_t N>
std::string_view nameOf(const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value) {
    const auto it =
        std::find_if(table.begin(), table.end(), [&](const auto& entry) { return entry.first == value; });
    return it == table.end() ? std::string_view{} : it->second;
}

template <typename Enum, std::size_t N>
std::optional<Enum> valueNamed(const std::array<std::pair<Enum, std::string_view>, N>& table,
                               const Json& field) {
    if (!field.is_string()) {
        return std::nullopt;
    }
    const auto& name = field.get_ref<const std::string&>();
    const auto it =
        std::find_if(table.begin(), table.end(), [&](const auto& entry) { return entry.second == name; });
    return it == table.end() ? std::nullopt : std::optional<Enum>(it->first);
}

// A whole number goes out without a fraction, so a range reads as the codes it spans: [0,1023], not
// [0.0,1023.0]. Beyond 2^53 a double holds only whole numbers anyway, and is written as one.
nlohmann::ordered_json rangeField(const ValueRange& range) {
    constexpr double kLargestExactInteger = 9007199254740992.0; // 2^53
    auto field = nlohmann::ordered_json::array();
    for (const double value : {range.min, range.max}) {
        if (value == std::trunc(value) && std::abs(value) <= kLargestExactInteger) {
            field.push_back(static_cast<std::int64_t>(value));
        } else {
            field.push_back(value);
        }
    }
    return field;
}

// Two JSON numbers, integers or not, with min < max. JSON cannot carry a number that is not finite.
std::optional<ValueRange> readRange(const Json& field) {
    if (!field.is_array() || field.size() != 2 || !field[0].is_number() || !field[1].is_number()) {
        return std::nullopt;
    }
    const ValueRange range{.min = field[0].get<double>(), .max = field[1].get<double>()};
    return range.empty() ? std::nullopt : std::optional<ValueRange>(range);
}

const DepthInfo* findDepth(int depth) {
    const auto it =
        std::find_if(kDepths.begin(), kDepths.end(), [&](const auto& d) { return d.depth == depth; });
    return it == kDepths.end() ? nullptr : &*it;
}

const DepthInfo* findDepth(std::string_view name) {
    const auto it =
        std::find_if(kDepths.begin(), kDepths.end(), [&](const auto& d) { return d.name == name; });
    return it == kDepths.end() ? nullptr : &*it;
}

// At most 32768 × 32768 × 4 channels × 8 bytes = 2^35, so the product cannot overflow 64 bits.
std::uint64_t pixelByteCount(int rows, int cols, int channels, const DepthInfo& depth) {
    return std::uint64_t(rows) * std::uint64_t(cols) * std::uint64_t(channels) * depth.elementSize;
}

// Length of the well-formed UTF-8 sequence starting at text[i], or 0 if there is none (RFC 3629: no
// overlong forms, no surrogates, nothing above U+10FFFF).
std::size_t utf8SequenceLength(std::string_view text, std::size_t i) {
    const auto byteAt = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byteAt(i);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char secondMin = 0x80;
    unsigned char secondMax = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead == 0xE0) {
        length = 3;
        secondMin = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        length = 3;
    } else if (lead == 0xED) {
        length = 3;
        secondMax = 0x9F;
    } else if (lead == 0xF0) {
        length = 4;
        secondMin = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        length = 4;
    } else if (lead == 0xF4) {
        length = 4;
        secondMax = 0x8F;
    } else {
        return 0;
    }
    if (i + length > text.size() || byteAt(i + 1) < secondMin || byteAt(i + 1) > secondMax) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if ((byteAt(i + k) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return length;
}

// Windows file names in an ANSI code page arrive here as invalid UTF-8, which JSON cannot carry.
std::string toValidUtf8(std::string_view text) {
    constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD"; // U+FFFD
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (const auto length = utf8SequenceLength(text, i); length > 0) {
            result.append(text.substr(i, length));
            i += length;
        } else {
            result.append(kReplacementCharacter);
            ++i;
        }
    }
    return result;
}

// Expects valid UTF-8; steps back over continuation bytes so no character is cut in half.
std::string truncateUtf8(std::string text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    text.resize(cut);
    return text;
}

// The parsed header is only ever inspected in place: copying or dumping a deeply nested document recurses
// once per level and can overflow a secondary thread's stack.

bool hasString(const Json& header, const char* key, std::string_view expected) {
    const auto it = header.find(key);
    return it != header.end() && it->is_string() && it->get_ref<const std::string&>() == expected;
}

// JSON integers only: booleans and floating-point numbers are rejected. The value is range-checked as a
// 64-bit integer, before any narrowing.
std::optional<std::int64_t> readInteger64(const Json& header, const char* key, std::int64_t min,
                                          std::int64_t max) {
    const auto it = header.find(key);
    if (it == header.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(max) || static_cast<std::int64_t>(value) < min) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    }
    const auto value = it->get<std::int64_t>();
    if (value < min || value > max) {
        return std::nullopt;
    }
    return value;
}

// Checked before narrowing, so 4294967297 cannot wrap to 1.
std::optional<int> readInteger(const Json& header, const char* key, int min, int max) {
    const auto value = readInteger64(header, key, min, max);
    return value ? std::optional<int>(static_cast<int>(*value)) : std::nullopt;
}

// --- Overlays
// -----------------------------------------------------------------------------------------------

template <class... F> struct Overloaded : F... {
    using F::operator()...;
};
template <class... F> Overloaded(F...) -> Overloaded<F...>;

using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view kHexDigits = "0123456789ABCDEF";

// A thousandth of a pixel is far below anything drawn, and keeps a float's noise out of the header.
double rounded(double value) { return std::round(value * 1000.0) / 1000.0; }

bool withinLimit(double value) { return std::isfinite(value) && std::abs(value) <= kMaxOverlayCoordinate; }

// Within the limit every whole number is exact as an integer, so it goes out without a fraction.
OrderedJson numberField(double value) {
    if (value == std::trunc(value)) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

// The rounded values, or nothing when any of them is one the viewer would refuse.
template <std::size_t N>
std::optional<std::array<double, N>> roundedWithinLimit(std::array<double, N> values) {
    for (double& value : values) {
        value = rounded(value);
        if (!withinLimit(value)) {
            return std::nullopt;
        }
    }
    return values;
}

OrderedJson withLabelAndColour(OrderedJson item, const std::string& label, const Colour& colour) {
    if (!label.empty()) {
        item["label"] = truncateUtf8(toValidUtf8(label), kMaxLabelBytes);
    }
    if (colour != kOverlayGreen) {
        std::string text = "#";
        for (const std::uint8_t component : {colour.r, colour.g, colour.b}) {
            text += kHexDigits[component >> 4];
            text += kHexDigits[component & 0x0F];
        }
        item["colour"] = text;
    }
    return item;
}

// One overlay as the header carries it, or nothing for one the viewer would refuse (checked after rounding,
// so a radius of 0.0004 is refused here as the 0 it would arrive as).
std::optional<OrderedJson> overlayField(const Overlay& overlay) {
    return std::visit(
        Overloaded{
            [](const Polygon& polygon) -> std::optional<OrderedJson> {
                if (polygon.points.size() < 3 || polygon.points.size() > kMaxPolygonPoints) {
                    return std::nullopt;
                }
                auto points = OrderedJson::array();
                for (const cv::Point2d& point : polygon.points) {
                    const auto xy = roundedWithinLimit(std::array{point.x, point.y});
                    if (!xy) {
                        return std::nullopt;
                    }
                    points.push_back(OrderedJson::array({numberField((*xy)[0]), numberField((*xy)[1])}));
                }
                OrderedJson item = OrderedJson::object();
                item["shape"] = "polygon";
                item["points"] = std::move(points);
                return withLabelAndColour(std::move(item), polygon.label, polygon.colour);
            },
            [](const Rectangle& rectangle) -> std::optional<OrderedJson> {
                const auto& r = rectangle.rect;
                const auto values = roundedWithinLimit(std::array{r.x, r.y, r.width, r.height});
                if (!values || (*values)[2] <= 0.0 || (*values)[3] <= 0.0) {
                    return std::nullopt;
                }
                OrderedJson item = OrderedJson::object();
                item["shape"] = "rectangle";
                item["x"] = numberField((*values)[0]);
                item["y"] = numberField((*values)[1]);
                item["width"] = numberField((*values)[2]);
                item["height"] = numberField((*values)[3]);
                return withLabelAndColour(std::move(item), rectangle.label, rectangle.colour);
            },
            [](const Circle& circle) -> std::optional<OrderedJson> {
                const auto values =
                    roundedWithinLimit(std::array{circle.centre.x, circle.centre.y, circle.radius});
                if (!values || (*values)[2] <= 0.0) {
                    return std::nullopt;
                }
                OrderedJson item = OrderedJson::object();
                item["shape"] = "circle";
                item["x"] = numberField((*values)[0]);
                item["y"] = numberField((*values)[1]);
                item["radius"] = numberField((*values)[2]);
                return withLabelAndColour(std::move(item), circle.label, circle.colour);
            },
            [](const Point& point) -> std::optional<OrderedJson> {
                const auto values = roundedWithinLimit(std::array{point.at.x, point.at.y});
                if (!values) {
                    return std::nullopt;
                }
                OrderedJson item = OrderedJson::object();
                item["shape"] = "point";
                item["x"] = numberField((*values)[0]);
                item["y"] = numberField((*values)[1]);
                return withLabelAndColour(std::move(item), point.label, point.colour);
            },
        },
        overlay);
}

// The overlays the viewer would accept, at most kMaxOverlays of them; an empty array when none is left.
OrderedJson overlaysField(const std::vector<Overlay>& overlays) {
    auto field = OrderedJson::array();
    for (const Overlay& overlay : overlays) {
        if (field.size() == kMaxOverlays) {
            break;
        }
        if (auto item = overlayField(overlay)) {
            field.push_back(std::move(*item));
        }
    }
    return field;
}

constexpr const char* kOverlaysReason = "overlays is not a list of at most 256 shapes";
constexpr const char* kShapeReason = "overlay is not an object with a known shape";
constexpr const char* kGeometryReason = "overlay geometry is missing or out of range";
constexpr const char* kPolygonReason = "polygon does not have 3 to 1024 points";
constexpr const char* kLabelReason = "overlay label is not a string of at most 64 bytes";
constexpr const char* kColourReason = "overlay colour is not #RRGGBB";

// Every read below takes a reference into the parsed header and looks up only the keys it expects: a copy of
// a deeply nested value, or a walk over an unknown key's, would recurse once per level on the receive thread.
std::optional<double> readCoordinate(const Json& value) {
    if (!value.is_number()) {
        return std::nullopt;
    }
    const double number = value.get<double>();
    return withinLimit(number) ? std::optional<double>(number) : std::nullopt;
}

std::optional<double> readCoordinate(const Json& item, const char* key) {
    const auto it = item.find(key);
    return it == item.end() ? std::nullopt : readCoordinate(*it);
}

// Above 0 once rounded to a thousandth of a pixel, as the sender rounds: what rounds to 0 would be left out
// the next time the image is encoded, so it is refused here rather than accepted and later lost.
std::optional<double> readSize(const Json& item, const char* key) {
    const auto value = readCoordinate(item, key);
    return value && rounded(*value) > 0.0 ? value : std::nullopt;
}

int hexValue(char digit) {
    const auto position = kHexDigits.find(static_cast<char>(std::toupper(static_cast<unsigned char>(digit))));
    return position == std::string_view::npos ? -1 : static_cast<int>(position);
}

std::optional<Colour> readColour(const Json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 7 || text[0] != '#') {
        return std::nullopt;
    }
    std::array<std::uint8_t, 3> components{};
    for (std::size_t i = 0; i < components.size(); ++i) {
        const int high = hexValue(text[1 + 2 * i]);
        const int low = hexValue(text[2 + 2 * i]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        components[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return Colour{components[0], components[1], components[2]};
}

std::variant<std::vector<cv::Point2d>, DecodeError> readPolygonPoints(const Json& item) {
    const auto field = item.find("points");
    if (field == item.end() || !field->is_array() || field->size() < 3 || field->size() > kMaxPolygonPoints) {
        return DecodeError{kPolygonReason};
    }
    std::vector<cv::Point2d> points;
    points.reserve(field->size());
    for (const Json& point : *field) {
        if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) {
            return DecodeError{kPolygonReason};
        }
        const auto x = readCoordinate(point[0]);
        const auto y = readCoordinate(point[1]);
        if (!x || !y) {
            return DecodeError{kGeometryReason};
        }
        points.emplace_back(*x, *y);
    }
    return points;
}

std::variant<Overlay, DecodeError> readOverlay(const Json& item) {
    const auto shape = item.is_object() ? item.find("shape") : item.end();
    if (!item.is_object() || shape == item.end() || !shape->is_string()) {
        return DecodeError{kShapeReason};
    }
    const auto& shapeName = shape->get_ref<const std::string&>();
    Overlay overlay;
    if (shapeName == "polygon") {
        auto points = readPolygonPoints(item);
        if (const auto* error = std::get_if<DecodeError>(&points)) {
            return *error;
        }
        overlay = Polygon{.points = std::get<std::vector<cv::Point2d>>(std::move(points))};
    } else if (shapeName == "rectangle") {
        const auto x = readCoordinate(item, "x");
        const auto y = readCoordinate(item, "y");
        const auto width = readSize(item, "width");
        const auto height = readSize(item, "height");
        if (!x || !y || !width || !height) {
            return DecodeError{kGeometryReason};
        }
        overlay = Rectangle{.rect = {*x, *y, *width, *height}};
    } else if (shapeName == "circle") {
        const auto x = readCoordinate(item, "x");
        const auto y = readCoordinate(item, "y");
        const auto radius = readSize(item, "radius");
        if (!x || !y || !radius) {
            return DecodeError{kGeometryReason};
        }
        overlay = Circle{.centre = {*x, *y}, .radius = *radius};
    } else if (shapeName == "point") {
        const auto x = readCoordinate(item, "x");
        const auto y = readCoordinate(item, "y");
        if (!x || !y) {
            return DecodeError{kGeometryReason};
        }
        overlay = Point{.at = {*x, *y}};
    } else {
        return DecodeError{kShapeReason};
    }

    std::string label;
    if (const auto field = item.find("label"); field != item.end()) {
        if (!field->is_string() || field->get_ref<const std::string&>().size() > kMaxLabelBytes) {
            return DecodeError{kLabelReason};
        }
        label = field->get_ref<const std::string&>();
    }
    Colour colour = kOverlayGreen;
    if (const auto field = item.find("colour"); field != item.end()) {
        const auto read = readColour(*field);
        if (!read) {
            return DecodeError{kColourReason};
        }
        colour = *read;
    }
    std::visit(
        [&](auto& shapeValue) {
            shapeValue.label = std::move(label);
            shapeValue.colour = colour;
        },
        overlay);
    return overlay;
}

std::variant<std::vector<Overlay>, DecodeError> readOverlays(const Json& field) {
    if (!field.is_array() || field.size() > kMaxOverlays) {
        return DecodeError{kOverlaysReason};
    }
    std::vector<Overlay> overlays;
    overlays.reserve(field.size());
    for (const Json& item : field) {
        auto overlay = readOverlay(item);
        if (const auto* error = std::get_if<DecodeError>(&overlay)) {
            return *error;
        }
        overlays.push_back(std::get<Overlay>(std::move(overlay)));
    }
    return overlays;
}

// --- Decoding
// -----------------------------------------------------------------------------------------------

std::variant<ImageMessage, DecodeError> decodeChecked(std::string_view headerText,
                                                      std::span<const std::byte> pixels) {
    if (headerText.size() > kMaxHeaderBytes) {
        return DecodeError{"header too large"};
    }
    // A malformed header is expected input from the network, not a programming error, so no exceptions.
    const Json header = Json::parse(headerText, nullptr, /*allow_exceptions=*/false);
    if (header.is_discarded()) {
        return DecodeError{"header is not valid JSON"};
    }
    if (!header.is_object()) {
        return DecodeError{"header is not a JSON object"};
    }
    if (!hasString(header, "protocol", kProtocolName)) {
        return DecodeError{"unknown protocol"};
    }
    if (readInteger(header, "version", 0, std::numeric_limits<int>::max()) != kProtocolVersion) {
        return DecodeError{"unsupported version"};
    }
    if (!hasString(header, "type", kImageType)) {
        return DecodeError{"unsupported message type"};
    }

    const auto name = header.find("name");
    if (name == header.end() || !name->is_string()) {
        return DecodeError{"name is missing or not a string"};
    }
    const auto& nameText = name->get_ref<const std::string&>();
    if (nameText.size() > kMaxNameBytes) {
        return DecodeError{"name too long"};
    }

    const auto rows = readInteger(header, "rows", 1, kMaxDimension);
    if (!rows) {
        return DecodeError{"rows out of range"};
    }
    const auto cols = readInteger(header, "cols", 1, kMaxDimension);
    if (!cols) {
        return DecodeError{"cols out of range"};
    }
    const auto depthField = header.find("depth");
    const DepthInfo* depth = (depthField != header.end() && depthField->is_string())
                                 ? findDepth(depthField->get_ref<const std::string&>())
                                 : nullptr;
    if (depth == nullptr) {
        return DecodeError{"unsupported depth"};
    }
    const auto channels = readInteger(header, "channels", 1, kMaxChannels);
    if (!channels) {
        return DecodeError{"channels out of range"};
    }

    // Both optional, on any image; an older sender sends neither.
    bool live = false;
    if (const auto field = header.find("live"); field != header.end()) {
        if (!field->is_boolean()) {
            return DecodeError{"live is not a boolean"};
        }
        live = field->get<bool>();
    }
    std::optional<SentTime> sentAt;
    if (header.contains("sentAt")) {
        const auto milliseconds = readInteger64(header, "sentAt", 0, kMaxSentAtMilliseconds);
        if (!milliseconds) {
            return DecodeError{"sentAt out of range"};
        }
        sentAt = SentTime{std::chrono::milliseconds(*milliseconds)};
    }

    // The image description: every field optional, on any image.
    ImageDescription description;
    if (const auto field = header.find("valueRange"); field != header.end()) {
        const auto range = readRange(*field);
        if (!range) {
            return DecodeError{"valueRange is not two increasing numbers"};
        }
        description.valueRange = *range;
    }
    if (const auto field = header.find("transfer"); field != header.end()) {
        description.transfer = valueNamed(kTransfers, *field);
        if (!description.transfer) {
            return DecodeError{"transfer is not sdr, hlg or pq"};
        }
    }
    if (const auto field = header.find("primaries"); field != header.end()) {
        description.primaries = valueNamed(kPrimaries, *field);
        if (!description.primaries) {
            return DecodeError{"primaries is not bt709 or bt2020"};
        }
    }
    if (const auto field = header.find("overlays"); field != header.end()) {
        auto overlays = readOverlays(*field);
        if (const auto* error = std::get_if<DecodeError>(&overlays)) {
            return *error;
        }
        description.overlays = std::get<std::vector<Overlay>>(std::move(overlays));
    }

    const auto expectedBytes = pixelByteCount(*rows, *cols, *channels, *depth);
    if (expectedBytes > kMaxPixelBytes) {
        return DecodeError{"image too large"};
    }
    if (pixels.size() != expectedBytes) {
        return DecodeError{"pixel frame has the wrong size"};
    }

    // cv::Mat has no read-only view type; the clone is the only access, and gives the image its own pixels.
    const cv::Mat view(*rows, *cols, CV_MAKETYPE(depth->depth, *channels),
                       const_cast<std::byte*>(pixels.data()));
    return ImageMessage{
        .name = nameText, .image = view.clone(), .live = live, .sentAt = sentAt, .description = description};
}

} // namespace

bool ValueRange::empty() const { return !(std::isfinite(min) && std::isfinite(max) && min < max); }

std::string sanitiseName(std::string_view name) { return truncateUtf8(toValidUtf8(name), kMaxNameBytes); }

std::span<const std::byte> EncodedMessage::pixelBytes() const {
    return {reinterpret_cast<const std::byte*>(pixels.data), pixels.total() * pixels.elemSize()};
}

bool isSupportedImage(const cv::Mat& image) {
    if (image.empty() || image.dims != 2 || image.rows > kMaxDimension || image.cols > kMaxDimension ||
        image.channels() > kMaxChannels) {
        return false;
    }
    const DepthInfo* depth = findDepth(image.depth());
    return depth != nullptr &&
           pixelByteCount(image.rows, image.cols, image.channels(), *depth) <= kMaxPixelBytes;
}

std::variant<EncodedMessage, EncodeError> encode(const cv::Mat& image, std::string_view name,
                                                 const EncodeOptions& options) {
    if (!isSupportedImage(image)) {
        return EncodeError{"image is empty, not 2-D, too large, or of an unsupported depth or channel count"};
    }
    // ordered_json keeps the documented field order, which makes captured traffic easier to read.
    nlohmann::ordered_json header = {
        {"protocol", kProtocolName},
        {"version", kProtocolVersion},
        {"type", kImageType},
        {"name", sanitiseName(name)},
        {"rows", image.rows},
        {"cols", image.cols},
        {"depth", findDepth(image.depth())->name},
        {"channels", image.channels()},
    };
    if (options.live) {
        header["live"] = true;
    }
    // A clock set before 1970 or past 9999 sends no sentAt rather than one the viewer refuses.
    if (options.sentAt) {
        const auto milliseconds = options.sentAt->time_since_epoch().count();
        if (milliseconds >= 0 && milliseconds <= kMaxSentAtMilliseconds) {
            header["sentAt"] = milliseconds;
        }
    }
    const auto& description = options.description;
    if (!description.valueRange.empty()) {
        header["valueRange"] = rangeField(description.valueRange);
    }
    if (description.transfer) {
        header["transfer"] = nameOf(kTransfers, *description.transfer);
    }
    if (description.primaries) {
        header["primaries"] = nameOf(kPrimaries, *description.primaries);
    }
    if (auto overlays = overlaysField(description.overlays); !overlays.empty()) {
        // As many overlays as fit 64 KiB, from the first: the trailing ones are left out like invalid ones,
        // rather than the image refused. Each item is measured once, so this is linear in the header's size.
        constexpr std::size_t kFieldBytes = std::string_view(R"(,"overlays":[])").size();
        std::size_t bytes = header.dump().size() + kFieldBytes;
        std::size_t fit = 0;
        for (const auto& item : overlays) {
            const std::size_t itemBytes = item.dump().size() + (fit > 0 ? 1 : 0); // and its comma
            if (bytes + itemBytes > kMaxHeaderBytes) {
                break;
            }
            bytes += itemBytes;
            ++fit;
        }
        if (fit > 0) {
            overlays.erase(overlays.begin() + static_cast<std::ptrdiff_t>(fit), overlays.end());
            header["overlays"] = std::move(overlays);
        }
    }

    EncodedMessage message;
    message.header = header.dump();
    // Only overlays could make it this large, and they were cut to fit; kept so that no header the viewer
    // would refuse is ever sent.
    if (message.header.size() > kMaxHeaderBytes) {
        return EncodeError{"header too large"};
    }
    // A region of interest keeps its parent's row stride; the wire carries tightly packed rows.
    message.pixels = image.isContinuous() ? image : image.clone();
    return message;
}

std::variant<ImageMessage, DecodeError> decode(std::string_view header, std::span<const std::byte> pixels) {
    try {
        return decodeChecked(header, pixels);
    } catch (const std::exception&) {
        // Only allocation can fail past validation; report it rather than let it escape a receive loop.
        return DecodeError{"could not decode message"};
    }
}

} // namespace visual_debug
