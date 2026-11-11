#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace doc_vision {

// One output of a model run: its name, its shape and its values.
struct Tensor {
    std::string name;
    std::vector<std::int64_t> shape;
    std::vector<float> data;
};

// Why a model could not be opened: ONNX Runtime itself (older than the version built against; a missing one
// stops the program from starting, as it is linked), or the model file (missing, unreadable, not a model).
enum class OpenFailure { None, Runtime, Model };

// ONNX Runtime's own choice of intra-op threads (its physical cores): LC100 was fastest so on the Mac mini M4
// Pro, against 1 and 4 threads (tests/doc_vision/doc_vision_eval.cpp, --threads).
inline constexpr int kDefaultIntraOpThreads = 0;

// The ONNX Runtime version loaded at run time, such as "1.31.1"; empty, with `error`, when the loaded library
// does not offer the API this was built against. On Windows an older onnxruntime.dll ships in System32, which
// the loader takes when the project's copy is not beside the program.
std::string runtimeVersion(std::string& error);

// An ONNX model on ONNX Runtime's CPU execution provider. No exception leaves it: errors come back as values.
class OnnxModel {
public:
    // Null, with `error` (and `failure`, when given), when ONNX Runtime cannot be used, or the file is
    // missing or not a model. `intraOpThreads` 0 is ONNX Runtime's own default.
    static std::unique_ptr<OnnxModel> open(const std::filesystem::path& file, std::string& error,
                                           int intraOpThreads = kDefaultIntraOpThreads,
                                           OpenFailure* failure = nullptr);
    ~OnnxModel();
    OnnxModel(const OnnxModel&) = delete;
    OnnxModel& operator=(const OnnxModel&) = delete;

    // Every output for one float input of `shape`, in the model's output order; empty, with `error`, on
    // failure.
    std::vector<Tensor> run(const std::vector<float>& input, const std::vector<std::int64_t>& shape,
                            std::string& error);

private:
    struct Impl;
    explicit OnnxModel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace doc_vision
