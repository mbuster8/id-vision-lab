// The golden headers in tests/wire/header_cases.json, which the Python sender's tests check too: encode()
// must write every case's header exactly, so the two senders stay in step. A case whose expected value is "?"
// makes this test print what C++ writes for it and fail: that is how every expected header in the file was
// obtained.

#include "visual_debug/overlay.h"
#include "visual_debug/wire_format.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Json = nlohmann::json;
using visual_debug::Colour;
using visual_debug::EncodedMessage;
using visual_debug::EncodeError;
using visual_debug::EncodeOptions;
using visual_debug::Overlay;

Json loadCases() {
    std::ifstream file(ID_VISION_LAB_HEADER_CASES, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "cannot open " << ID_VISION_LAB_HEADER_CASES;
        return {};
    }
    Json cases = Json::parse(file, nullptr, false);
    if (cases.is_discarded() || !cases.contains("cases") || !cases["cases"].is_array()) {
        ADD_FAILURE() << ID_VISION_LAB_HEADER_CASES << " is not a JSON object with a cases list";
        return {};
    }
    return cases["cases"];
}

// A JSON number, or one of the strings that stand for what JSON cannot hold.
double numberFrom(const Json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    const std::string text = value.is_string() ? value.get<std::string>() : std::string();
    if (text == "nan") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text == "inf") {
        return std::numeric_limits<double>::infinity();
    }
    if (text == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    ADD_FAILURE() << "not a number: " << value.dump();
    return 0.0;
}

std::uint8_t hexByte(const std::string& text, std::size_t at) {
    return static_cast<std::uint8_t>(std::stoul(text.substr(at, 2), nullptr, 16));
}

Colour colourFrom(const Json& item) {
    if (!item.contains("colour")) {
        return visual_debug::kOverlayGreen;
    }
    const std::string text = item["colour"].get<std::string>(); // "#RRGGBB"
    return Colour{hexByte(text, 1), hexByte(text, 3), hexByte(text, 5)};
}

Overlay overlayFrom(const Json& item) {
    const std::string shape = item.at("shape").get<std::string>();
    const std::string label = item.value("label", std::string());
    const Colour colour = colourFrom(item);
    if (shape == "polygon") {
        std::vector<cv::Point2d> points;
        for (const Json& point : item.at("points")) {
            points.emplace_back(numberFrom(point.at(0)), numberFrom(point.at(1)));
        }
        return visual_debug::Polygon{points, label, colour};
    }
    if (shape == "rectangle") {
        return visual_debug::Rectangle{cv::Rect2d(numberFrom(item.at("x")), numberFrom(item.at("y")),
                                                  numberFrom(item.at("width")),
                                                  numberFrom(item.at("height"))),
                                       label, colour};
    }
    if (shape == "circle") {
        return visual_debug::Circle{cv::Point2d(numberFrom(item.at("x")), numberFrom(item.at("y"))),
                                    numberFrom(item.at("radius")), label, colour};
    }
    if (shape != "point") {
        ADD_FAILURE() << "unknown shape: " << shape;
    }
    return visual_debug::Point{cv::Point2d(numberFrom(item.at("x")), numberFrom(item.at("y"))), label,
                               colour};
}

// count polygons of `points` points [i, i + 0.5], generated as the Python test generates them.
std::vector<Overlay> repeatedPolygons(const Json& repeat) {
    std::vector<Overlay> overlays;
    const int count = repeat.at("count").get<int>();
    const int points = repeat.at("points").get<int>();
    for (int k = 0; k < count; ++k) {
        visual_debug::Polygon polygon;
        for (int i = 0; i < points; ++i) {
            polygon.points.emplace_back(i, i + 0.5);
        }
        overlays.emplace_back(std::move(polygon));
    }
    return overlays;
}

std::string nameFrom(const Json& testCase) {
    if (!testCase.contains("nameHex")) {
        return testCase.value("name", std::string("x"));
    }
    const std::string hex = testCase["nameHex"].get<std::string>();
    std::string name;
    for (std::size_t at = 0; at + 1 < hex.size(); at += 2) {
        name += static_cast<char>(hexByte(hex, at));
    }
    return name;
}

int depthFrom(const std::string& name) {
    const std::pair<const char*, int> depths[] = {{"8U", CV_8U},   {"8S", CV_8S},   {"16U", CV_16U},
                                                  {"16S", CV_16S}, {"32S", CV_32S}, {"32F", CV_32F},
                                                  {"64F", CV_64F}};
    for (const auto& [text, depth] : depths) {
        if (name == text) {
            return depth;
        }
    }
    ADD_FAILURE() << "unknown depth: " << name;
    return CV_8U;
}

EncodeOptions optionsFrom(const Json& testCase) {
    EncodeOptions options;
    options.live = testCase.value("live", false);
    if (testCase.contains("sentAt")) {
        options.sentAt =
            visual_debug::SentTime{std::chrono::milliseconds(testCase["sentAt"].get<std::int64_t>())};
    }
    auto& description = options.description;
    if (testCase.contains("valueRange")) {
        description.valueRange = {numberFrom(testCase["valueRange"].at(0)),
                                  numberFrom(testCase["valueRange"].at(1))};
    }
    if (testCase.contains("transfer")) {
        const std::string transfer = testCase["transfer"].get<std::string>();
        description.transfer = transfer == "hlg"  ? visual_debug::Transfer::Hlg
                               : transfer == "pq" ? visual_debug::Transfer::Pq
                                                  : visual_debug::Transfer::Sdr;
    }
    if (testCase.contains("primaries")) {
        description.primaries = testCase["primaries"].get<std::string>() == "bt2020"
                                    ? visual_debug::Primaries::Bt2020
                                    : visual_debug::Primaries::Bt709;
    }
    if (testCase.contains("overlays")) {
        for (const Json& item : testCase["overlays"]) {
            description.overlays.push_back(overlayFrom(item));
        }
    }
    if (testCase.contains("overlaysRepeat")) {
        description.overlays = repeatedPolygons(testCase["overlaysRepeat"]);
    }
    return options;
}

TEST(HeaderCases, EncodeWritesEveryGoldenHeader) {
    const Json cases = loadCases();
    ASSERT_FALSE(cases.empty());
    for (const Json& testCase : cases) {
        const std::string id = testCase.at("id").get<std::string>();
        const int type =
            CV_MAKETYPE(depthFrom(testCase.value("depth", std::string("8U"))), testCase.value("channels", 1));
        const cv::Mat image = cv::Mat::zeros(testCase.value("rows", 1), testCase.value("cols", 1), type);
        auto result = visual_debug::encode(image, nameFrom(testCase), optionsFrom(testCase));
        const auto* message = std::get_if<EncodedMessage>(&result);
        ASSERT_NE(message, nullptr) << id << ": " << std::get<EncodeError>(result).reason;

        if (testCase.contains("header")) {
            const std::string expected = testCase["header"].get<std::string>();
            if (expected == "?") {
                ADD_FAILURE() << "GOLDEN\t" << id << "\t" << message->header;
            } else {
                EXPECT_EQ(message->header, expected) << id;
            }
            continue;
        }
        // A generated case too long to keep whole: how many shapes were kept, and the header's size.
        const Json header = Json::parse(message->header);
        const std::size_t kept = header.contains("overlays") ? header["overlays"].size() : 0;
        if (testCase.at("keptShapes") == "?") {
            ADD_FAILURE() << "GOLDEN\t" << id << "\t"
                          << Json{{"keptShapes", kept}, {"headerBytes", message->header.size()}}.dump();
            continue;
        }
        EXPECT_EQ(kept, testCase["keptShapes"].get<std::size_t>()) << id;
        EXPECT_EQ(message->header.size(), testCase.at("headerBytes").get<std::size_t>()) << id;
    }
}

} // namespace
