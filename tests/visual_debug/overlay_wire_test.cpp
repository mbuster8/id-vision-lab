#include "visual_debug/overlay.h"
#include "visual_debug/wire_format.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <cmath>
#include <limits>
#include <ostream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using visual_debug::Circle;
using visual_debug::DecodeError;
using visual_debug::EncodedMessage;
using visual_debug::EncodeError;
using visual_debug::ImageDescription;
using visual_debug::ImageMessage;
using visual_debug::kOverlayGreen;
using visual_debug::kOverlayRed;
using visual_debug::Overlay;
using visual_debug::Point;
using visual_debug::Polygon;
using visual_debug::Rectangle;
using Json = nlohmann::ordered_json;

EncodedMessage encodeOrFail(const ImageDescription& description) {
    auto result =
        visual_debug::encode(cv::Mat(4, 4, CV_8UC1, cv::Scalar(0)), "card", {.description = description});
    if (const auto* error = std::get_if<EncodeError>(&result)) {
        ADD_FAILURE() << "encode failed: " << error->reason;
        return {};
    }
    return std::get<EncodedMessage>(std::move(result));
}

std::vector<Overlay> roundTrip(const ImageDescription& description) {
    const auto encoded = encodeOrFail(description);
    auto decoded = visual_debug::decode(encoded.header, encoded.pixelBytes());
    if (const auto* error = std::get_if<DecodeError>(&decoded)) {
        ADD_FAILURE() << "decode failed: " << error->reason;
        return {};
    }
    return std::get<ImageMessage>(std::move(decoded)).description.overlays;
}

// The smallest valid header, as wire_format_test.cpp builds it, for raw-text cases.
Json validHeader() {
    return {{"protocol", "visual_debug"},
            {"version", 2},
            {"type", "image"},
            {"name", "test"},
            {"rows", 2},
            {"cols", 3},
            {"depth", "8U"},
            {"channels", 1}};
}
constexpr std::size_t kValidPixelBytes = 2 * 3;

std::variant<ImageMessage, DecodeError> decodeHeader(const std::string& header) {
    const std::vector<std::byte> pixels(kValidPixelBytes);
    return visual_debug::decode(header, pixels);
}

std::variant<ImageMessage, DecodeError> decodeWithOverlays(Json overlays) {
    auto header = validHeader();
    header["overlays"] = std::move(overlays);
    return decodeHeader(header.dump());
}

TEST(OverlayWire, EveryShapeRoundTrips) {
    const ImageDescription description{
        .overlays = {Polygon{{{412.5, 120.25}, {1630, 96}, {1655, 1010}, {398, 1042}}, "Passport"},
                     Rectangle{{470, 300, 260, 330}, "Photo"},
                     Polygon{{{420, 860}, {1640, 832}, {1646, 990}}, "MRZ", kOverlayRed},
                     Circle{{600, 465}, 140}, Point{{412.5, 120.25}, "", {1, 2, 3}}}};

    EXPECT_EQ(roundTrip(description), description.overlays);
}

TEST(OverlayWire, WritesTheDocumentedJson) {
    const auto encoded =
        encodeOrFail({.overlays = {Rectangle{{1, 2, 3, 4}, "MRZ", kOverlayRed}, Point{{0.5, 7}}}});

    const std::string expectedEnd =
        R"(,"overlays":[{"shape":"rectangle","x":1,"y":2,"width":3,"height":4,"label":"MRZ",)"
        R"("colour":"#E05050"},{"shape":"point","x":0.5,"y":7}]})";
    ASSERT_GE(encoded.header.size(), expectedEnd.size());
    EXPECT_EQ(encoded.header.substr(encoded.header.size() - expectedEnd.size()), expectedEnd)
        << encoded.header;
}

// The example in docs/wire-format.md, word for word.
TEST(OverlayWire, WritesTheWireFormatExample) {
    const ImageDescription description{
        .overlays = {Polygon{{{210, 170}, {1390, 140}, {1410, 880}, {190, 900}}, "Document"},
                     Polygon{{{250, 755}, {1350, 745}, {1352, 862}, {252, 872}}, "MRZ", kOverlayRed}}};

    const auto result = visual_debug::encode(cv::Mat(1000, 1600, CV_8UC3), "card with overlays",
                                             {.description = description});

    ASSERT_TRUE(std::holds_alternative<EncodedMessage>(result));
    EXPECT_EQ(
        std::get<EncodedMessage>(result).header,
        R"({"protocol":"visual_debug","version":2,"type":"image","name":"card with overlays","rows":1000,)"
        R"("cols":1600,"depth":"8U","channels":3,"overlays":[{"shape":"polygon","points":[[210,170],)"
        R"([1390,140],[1410,880],[190,900]],"label":"Document"},{"shape":"polygon","points":[[250,755],)"
        R"([1350,745],[1352,862],[252,872]],"label":"MRZ","colour":"#E05050"}]})");
}

TEST(OverlayWire, WritesOverlaysAfterPrimaries) {
    const auto encoded =
        encodeOrFail({.primaries = visual_debug::Primaries::Bt709, .overlays = {Point{{1, 2}}}});

    EXPECT_LT(encoded.header.find("\"primaries\""), encoded.header.find("\"overlays\""));
}

TEST(OverlayWire, RoundsCoordinatesToAThousandthOfAPixel) {
    const auto overlays = roundTrip({.overlays = {Point{{0.1234567, 2}}}});

    ASSERT_EQ(overlays.size(), 1u);
    EXPECT_EQ(std::get<Point>(overlays[0]).at.x, 0.123);
}

TEST(OverlayWire, NoOverlaysWritesNoField) {
    EXPECT_EQ(encodeOrFail({}).header.find("overlays"), std::string::npos);
}

TEST(OverlayWire, LeavesOutWhatTheViewerWouldRefuse) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<cv::Point2d> tooMany(1025, cv::Point2d(1, 1));
    const ImageDescription description{.overlays = {
                                           Point{{nan, 1}},
                                           Point{{2e6, 1}},
                                           Rectangle{{0, 0, 0, 5}},
                                           Rectangle{{0, 0, 0.0004, 5}},
                                           Circle{{5, 5}, -1},
                                           Circle{{5, 5}, 0.0004},
                                           Polygon{{{0, 0}, {1, 1}}},
                                           Polygon{tooMany},
                                           Point{{3, 4}, "kept"},
                                       }};

    EXPECT_EQ(roundTrip(description), (std::vector<Overlay>{Point{{3, 4}, "kept"}}));
}

TEST(OverlayWire, AllLeftOutWritesNoField) {
    EXPECT_EQ(encodeOrFail({.overlays = {Circle{{5, 5}, 0}}}).header.find("overlays"), std::string::npos);
}

TEST(OverlayWire, SendsAtMost256) {
    ImageDescription description;
    for (int i = 0; i < 257; ++i) {
        description.overlays.push_back(Point{{static_cast<double>(i), 0}});
    }

    const auto overlays = roundTrip(description);

    ASSERT_EQ(overlays.size(), visual_debug::kMaxOverlays);
    EXPECT_EQ(std::get<Point>(overlays.back()).at.x, 255.0);
}

TEST(OverlayWire, CutsALongLabelAtACharacterBoundary) {
    std::string label;
    for (int i = 0; i < 70; ++i) {
        label += "\xC3\xA9"; // é
    }

    const auto overlays = roundTrip({.overlays = {Point{{1, 1}, label}}});

    ASSERT_EQ(overlays.size(), 1u);
    EXPECT_EQ(std::get<Point>(overlays[0]).label, label.substr(0, 64));
}

TEST(OverlayWire, MakesALabelValidUtf8) {
    const auto overlays = roundTrip({.overlays = {Point{{1, 1}, "MR\xFFZ"}}});

    ASSERT_EQ(overlays.size(), 1u);
    EXPECT_EQ(std::get<Point>(overlays[0]).label, "MR\xEF\xBF\xBDZ");
}

ImageDescription oversizedDescription() {
    ImageDescription description;
    for (int i = 0; i < 256; ++i) {
        description.overlays.push_back(Polygon{std::vector<cv::Point2d>(1000, cv::Point2d(123456.789, 1.5))});
    }
    return description;
}

TEST(OverlayWire, AHeaderOver64KiBIsNotEncoded) {
    const auto result =
        visual_debug::encode(cv::Mat(4, 4, CV_8UC1), "card", {.description = oversizedDescription()});

    ASSERT_TRUE(std::holds_alternative<EncodeError>(result));
    EXPECT_EQ(std::get<EncodeError>(result).reason, "header too large");
}

TEST(OverlayWire, ColourIsReadInEitherCase) {
    const auto decoded =
        decodeWithOverlays(Json::array({{{"shape", "point"}, {"x", 1}, {"y", 2}, {"colour", "#e05050"}},
                                        {{"shape", "point"}, {"x", 1}, {"y", 2}}}));

    ASSERT_TRUE(std::holds_alternative<ImageMessage>(decoded));
    const auto& overlays = std::get<ImageMessage>(decoded).description.overlays;
    ASSERT_EQ(overlays.size(), 2u);
    EXPECT_EQ(std::get<Point>(overlays[0]).colour, kOverlayRed);
    EXPECT_EQ(std::get<Point>(overlays[1]).colour, kOverlayGreen);
}

TEST(OverlayWire, UnknownKeysInAnOverlayAreIgnored) {
    const auto decoded = decodeWithOverlays(
        Json::array({{{"shape", "point"}, {"x", 1}, {"y", 2}, {"glow", Json::array({Json::array({1})})}}}));

    ASSERT_TRUE(std::holds_alternative<ImageMessage>(decoded));
    EXPECT_EQ(std::get<ImageMessage>(decoded).description.overlays, (std::vector<Overlay>{Point{{1, 2}}}));
}

TEST(OverlayWire, OlderFieldsStillDecodeBesideOverlays) {
    auto header = validHeader();
    header["valueRange"] = Json::array({16, 235});
    header["transfer"] = "hlg";
    header["overlays"] = Json::array({{{"shape", "circle"}, {"x", 1}, {"y", 2}, {"radius", 3}}});

    const auto decoded = decodeHeader(header.dump());

    ASSERT_TRUE(std::holds_alternative<ImageMessage>(decoded));
    const auto& description = std::get<ImageMessage>(decoded).description;
    EXPECT_EQ(description.valueRange, (visual_debug::ValueRange{16, 235}));
    EXPECT_EQ(description.transfer, visual_debug::Transfer::Hlg);
    EXPECT_EQ(description.overlays, (std::vector<Overlay>{Circle{{1, 2}, 3}}));
}

TEST(OverlayWire, ToPointsTakesOpenCvShapes) {
    EXPECT_EQ(visual_debug::toPoints(std::vector<cv::Point>{{1, 2}, {3, 4}}),
              (std::vector<cv::Point2d>{{1, 2}, {3, 4}}));
    EXPECT_EQ(visual_debug::toPoints(std::vector<cv::Point2f>{{1.5f, 2.f}}),
              (std::vector<cv::Point2d>{{1.5, 2}}));
    EXPECT_EQ(visual_debug::toPoints(cv::RotatedRect({10, 10}, {4, 2}, 0)),
              (std::vector<cv::Point2d>{{8, 11}, {8, 9}, {12, 9}, {12, 11}}));
}

// The receive thread's stack is small (512 KiB on macOS); a by-value copy of a deeply nested value inside an
// overlay would overflow it, as wire_format_test.cpp's whole-header case explains.
TEST(OverlayWire, DeeplyNestedValuesInsideOverlaysAreHandledOnASecondaryThread) {
    const std::string nested = std::string(30000, '[') + std::string(30000, ']');
    const std::string prefix = validHeader().dump();
    const std::string head = prefix.substr(0, prefix.size() - 1) + R"(,"overlays":[{"shape":"point",)";
    const std::vector<std::pair<std::string, std::string>> cases{
        {head + R"("x":1,"y":2,"glow":)" + nested + "}]}", ""},
        {head + R"("x":)" + nested + R"(,"y":2}]})", "overlay geometry is missing or out of range"},
        {head + R"("x":1,"y":2,"label":)" + nested + "}]}",
         "overlay label is not a string of at most 64 bytes"},
        {prefix.substr(0, prefix.size() - 1) + R"(,"overlays":[{"shape":"polygon","points":)" + nested +
             "}]}",
         "polygon does not have 3 to 1024 points"},
    };

    for (const auto& [header, reason] : cases) {
        ASSERT_LE(header.size(), visual_debug::kMaxHeaderBytes);
        std::string outcome;
        std::thread worker([&] {
            const auto decoded = decodeHeader(header);
            outcome =
                std::holds_alternative<DecodeError>(decoded) ? std::get<DecodeError>(decoded).reason : "";
        });
        worker.join();
        EXPECT_EQ(outcome, reason);
    }
}

struct OverlayRejection {
    std::string label;
    Json overlays;
    std::string reason;
};

void PrintTo(const OverlayRejection& rejection, std::ostream* out) { *out << rejection.label; }

Json point(Json x) { return {{"shape", "point"}, {"x", std::move(x)}, {"y", 2}}; }

Json polygonOf(std::size_t count) {
    Json points = Json::array();
    for (std::size_t i = 0; i < count; ++i) {
        points.push_back(Json::array({static_cast<int>(i), 0}));
    }
    return {{"shape", "polygon"}, {"points", std::move(points)}};
}

Json pointWith(const char* key, Json value) {
    Json item = point(1);
    item[key] = std::move(value);
    return item;
}

std::vector<OverlayRejection> overlayRejections() {
    const std::string kList = "overlays is not a list of at most 256 shapes";
    const std::string kShape = "overlay is not an object with a known shape";
    const std::string kGeometry = "overlay geometry is missing or out of range";
    const std::string kPolygon = "polygon does not have 3 to 1024 points";
    const std::string kLabel = "overlay label is not a string of at most 64 bytes";
    const std::string kColour = "overlay colour is not #RRGGBB";
    Json tooMany = Json::array();
    for (int i = 0; i < 257; ++i) {
        tooMany.push_back(point(i));
    }
    Json polygonWithShortPoint = polygonOf(3);
    polygonWithShortPoint["points"][1] = Json::array({1});
    Json polygonWithStringCoordinate = polygonOf(3);
    polygonWithStringCoordinate["points"][1] = Json::array({1, "2"});
    return {
        {"Object", Json::object(), kList},
        {"TooMany", tooMany, kList},
        {"NumberItem", Json::array({1}), kShape},
        {"NoShape", Json::array({{{"x", 1}}}), kShape},
        {"UnknownShape", Json::array({{{"shape", "line"}}}), kShape},
        {"MissingY", Json::array({{{"shape", "point"}, {"x", 1}}}), kGeometry},
        {"XAsString", Json::array({point("1")}), kGeometry},
        {"XBeyondAMillion", Json::array({point(1e7)}), kGeometry},
        {"ZeroWidth",
         Json::array({{{"shape", "rectangle"}, {"x", 1}, {"y", 1}, {"width", 0}, {"height", 1}}}), kGeometry},
        {"NegativeRadius", Json::array({{{"shape", "circle"}, {"x", 1}, {"y", 1}, {"radius", -2}}}),
         kGeometry},
        {"TwoPoints", Json::array({polygonOf(2)}), kPolygon},
        {"PointsOver1024", Json::array({polygonOf(1025)}), kPolygon},
        {"ShortPoint", Json::array({polygonWithShortPoint}), kPolygon},
        {"StringCoordinate", Json::array({polygonWithStringCoordinate}), kPolygon},
        {"LabelAsNumber", Json::array({pointWith("label", 5)}), kLabel},
        {"LabelOver64Bytes", Json::array({pointWith("label", std::string(65, 'a'))}), kLabel},
        {"ColourTooShort", Json::array({pointWith("colour", "#12345")}), kColour},
        {"ColourNotHex", Json::array({pointWith("colour", "#GG0000")}), kColour},
        {"ColourByName", Json::array({pointWith("colour", "red")}), kColour},
        {"ColourAsNumber", Json::array({pointWith("colour", 7)}), kColour},
    };
}

class OverlayWireRejection : public testing::TestWithParam<OverlayRejection> {};

TEST_P(OverlayWireRejection, RefusesMalformedOverlays) {
    const auto decoded = decodeWithOverlays(GetParam().overlays);

    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).reason, GetParam().reason);
}

INSTANTIATE_TEST_SUITE_P(EveryRule, OverlayWireRejection, testing::ValuesIn(overlayRejections()),
                         [](const testing::TestParamInfo<OverlayRejection>& info) {
                             return info.param.label;
                         });

} // namespace
