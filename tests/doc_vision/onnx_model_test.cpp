#include "doc_vision/onnx_model.h"

#include "scratch_folder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The model resolved at configure time, as UTF-8: the path may name a folder outside ASCII.
std::filesystem::path lc100Model() {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(ID_VISION_LAB_LC100_MODEL)));
}

TEST(OnnxModel, RuntimeVersionIsTheOneBuiltAgainst) {
    std::string error;
    EXPECT_EQ(doc_vision::runtimeVersion(error), ID_VISION_LAB_ONNXRUNTIME_VERSION);
    EXPECT_TRUE(error.empty()) << error;
}

TEST(OnnxModel, MissingFileIsAnErrorNamingIt) {
    std::string error;
    doc_vision::OpenFailure failure = doc_vision::OpenFailure::None;
    EXPECT_EQ(
        doc_vision::OnnxModel::open("no/such.onnx", error, doc_vision::kDefaultIntraOpThreads, &failure),
        nullptr);
    EXPECT_NE(error.find("such.onnx"), std::string::npos) << error;
    EXPECT_EQ(failure, doc_vision::OpenFailure::Model);
}

TEST(OnnxModel, NotAModelIsAnError) {
    const test_support::ScratchFolder folder;
    const std::filesystem::path file = folder.path() / "hello.onnx";
    std::ofstream(file) << "hello";
    std::string error;
    EXPECT_EQ(doc_vision::OnnxModel::open(file, error), nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(OnnxModel, RunsLc100) {
    std::string error;
    const auto model = doc_vision::OnnxModel::open(lc100Model(), error);
    ASSERT_NE(model, nullptr) << error;
    const std::vector<float> input(3 * 256 * 256, 0.5f);
    const std::vector<doc_vision::Tensor> outputs = model->run(input, {1, 3, 256, 256}, error);
    ASSERT_FALSE(outputs.empty()) << error;
    const doc_vision::Tensor* heatmap = &outputs.front();
    for (const doc_vision::Tensor& output : outputs) {
        if (output.name == "heatmap") {
            heatmap = &output;
        }
    }
    EXPECT_EQ(heatmap->shape, (std::vector<std::int64_t>{1, 4, 128, 128}));
    ASSERT_EQ(heatmap->data.size(), std::size_t{4 * 128 * 128});
    for (const float value : heatmap->data) {
        ASSERT_TRUE(std::isfinite(value));
    }
}

TEST(OnnxModel, WrongInputShapeIsAnError) {
    std::string error;
    const auto model = doc_vision::OnnxModel::open(lc100Model(), error);
    ASSERT_NE(model, nullptr) << error;
    const std::vector<float> input(3 * 10 * 10, 0.5f);
    EXPECT_TRUE(model->run(input, {1, 3, 256, 256}, error).empty());
    EXPECT_FALSE(error.empty());
}

} // namespace
