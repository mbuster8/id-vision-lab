#include "doc_vision/onnx_model.h"

// Homebrew installs the headers in include/onnxruntime and adds that folder; vcpkg adds include/.
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif

#include <cstddef>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace doc_vision {

namespace {

// A path as UTF-8 for a message: path::string() throws on Windows for a name the code page cannot hold, and
// no exception may leave doc_vision.
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// The API this was built against, checked before any ORT call. The C++ header is compiled with
// ORT_API_MANUAL_INIT, so nothing touches the API before this has.
struct Runtime {
    const OrtApi* api = nullptr;
    std::string error;
};

const Runtime& runtime() {
    static const Runtime instance = [] {
        Runtime r;
        const OrtApiBase* base = OrtGetApiBase();
        if (base == nullptr) {
            r.error = "ONNX Runtime could not be loaded";
            return r;
        }
        r.api = base->GetApi(ORT_API_VERSION);
        if (r.api == nullptr) {
            const char* loaded = base->GetVersionString();
            r.error = std::string("ONNX Runtime ") + (loaded != nullptr ? loaded : "?") + " is loaded; 1." +
                      std::to_string(ORT_API_VERSION) + ".x is needed";
            return r;
        }
        Ort::InitApi(r.api);
        return r;
    }();
    return instance;
}

// One environment for the process, made once the API is known to be there.
Ort::Env& environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "doc_vision");
    return env;
}

} // namespace

std::string runtimeVersion(std::string& error) {
    const Runtime& r = runtime();
    if (r.api == nullptr) {
        error = r.error;
        return {};
    }
    return OrtGetApiBase()->GetVersionString();
}

struct OnnxModel::Impl {
    Ort::Session session{nullptr};
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
};

OnnxModel::OnnxModel(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

OnnxModel::~OnnxModel() = default;

std::unique_ptr<OnnxModel> OnnxModel::open(const std::filesystem::path& file, std::string& error,
                                           int intraOpThreads, OpenFailure* failure) {
    const auto fail = [&](OpenFailure kind, std::string message) -> std::unique_ptr<OnnxModel> {
        error = std::move(message);
        if (failure != nullptr) {
            *failure = kind;
        }
        return nullptr;
    };
    const Runtime& r = runtime();
    if (r.api == nullptr) {
        return fail(OpenFailure::Runtime, r.error);
    }
    std::error_code exists;
    if (!std::filesystem::is_regular_file(file, exists)) {
        return fail(OpenFailure::Model, "no model file at " + utf8(file));
    }
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(intraOpThreads);
        options.SetInterOpNumThreads(1);
        // Workers that spin between runs keep cores busy in a program that mostly waits for the camera.
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto impl = std::make_unique<Impl>();
#ifdef _WIN32
        impl->session = Ort::Session(environment(), file.wstring().c_str(), options);
#else
        impl->session = Ort::Session(environment(), file.string().c_str(), options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        for (std::size_t i = 0; i < impl->session.GetInputCount(); ++i) {
            impl->inputNames.emplace_back(impl->session.GetInputNameAllocated(i, allocator).get());
        }
        for (std::size_t i = 0; i < impl->session.GetOutputCount(); ++i) {
            impl->outputNames.emplace_back(impl->session.GetOutputNameAllocated(i, allocator).get());
        }
        if (impl->inputNames.size() != 1 || impl->outputNames.empty()) {
            return fail(OpenFailure::Model, utf8(file.filename()) + ": not a model with one input");
        }
        if (failure != nullptr) {
            *failure = OpenFailure::None;
        }
        return std::unique_ptr<OnnxModel>(new OnnxModel(std::move(impl)));
    } catch (const std::exception& e) {
        return fail(OpenFailure::Model, utf8(file.filename()) + ": " + e.what());
    } catch (...) {
        return fail(OpenFailure::Model, utf8(file.filename()) + ": could not be opened");
    }
}

std::vector<Tensor> OnnxModel::run(const std::vector<float>& input, const std::vector<std::int64_t>& shape,
                                   std::string& error) {
    std::size_t expected = 1;
    for (const std::int64_t dimension : shape) {
        expected *= dimension > 0 ? static_cast<std::size_t>(dimension) : 0;
    }
    if (expected == 0 || expected != input.size()) {
        error = "the input holds " + std::to_string(input.size()) + " values; its shape asks for " +
                std::to_string(expected);
        return {};
    }
    try {
        const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        // Run never writes to its input; the C API takes a non-const pointer all the same.
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()),
                                                            input.size(), shape.data(), shape.size());
        const char* inputName = m_impl->inputNames.front().c_str();
        std::vector<const char*> outputNames;
        for (const std::string& name : m_impl->outputNames) {
            outputNames.push_back(name.c_str());
        }
        std::vector<Ort::Value> values = m_impl->session.Run(Ort::RunOptions{nullptr}, &inputName, &tensor, 1,
                                                             outputNames.data(), outputNames.size());
        std::vector<Tensor> outputs;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const Ort::TensorTypeAndShapeInfo info = values[i].GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                continue;
            }
            Tensor out;
            out.name = m_impl->outputNames[i];
            out.shape = info.GetShape();
            const float* data = values[i].GetTensorData<float>();
            out.data.assign(data, data + info.GetElementCount());
            outputs.push_back(std::move(out));
        }
        if (outputs.empty()) {
            error = "the model gave no float output";
        }
        return outputs;
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    } catch (...) {
        error = "the model could not run";
        return {};
    }
}

} // namespace doc_vision
