#pragma once

#include "doc_vision/onnx_model.h"
#include "doc_vision/page_geometry.h"

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace doc_vision {

// What one read of a flattened page found.
struct MrzReading {
    std::optional<Quad> band;       // on the page given, orderedCorners' order; none when no band was found
    std::vector<std::string> lines; // as recognised, raw; empty without a band
    // The detection's candidate, filled whether or not it passes as a band (the evaluation measures both
    // sides):
    double peak = 0.0;       // the heatmap's maximum, 0 to 1
    double widthShare = 0.0; // the candidate's long side over the page's width; 0 without a candidate
    double aspect = 0.0;     // its long side over its short side; 0 without a candidate
    std::chrono::nanoseconds detectTime{};
    std::chrono::nanoseconds recogniseTime{};
};

// "A band found" (spec §4.1): Otsu thresholds any heatmap, so the largest blob alone means nothing. Measured
// with doc_vision_eval --band-stats on 12 November: every read of a passport's MRZ (MIDV-2020's passport
// clips and the owner's) passed at a width share of 0.85; of 200 ID card fronts with no MRZ (MIDV-2020's
// Estonian and Slovak photos), none passed at 0.80 with the peak and aspect limits below, which alone
// separate nothing more.
inline constexpr double kBandPeak = 0.5;
inline constexpr double kBandWidthShare = 0.8;
inline constexpr double kBandAspect = 5.0;

// What reads an MRZ from a flattened page: MrzScanner in the lab; a scripted one in the tests.
class MrzReader {
public:
    virtual ~MrzReader() = default;
    // `page`: 8-bit BGR. No band and no lines, with `error`, when a model could not run.
    virtual MrzReading read(const cv::Mat& page, std::string& error) = 0;
    // From any thread: ends a read in progress at once; later reads fail at once.
    virtual void cancel() {}
};

// The reader's ONNX Runtime intra-op threads (LC100 keeps the runtime's default). Set by doc_vision_eval
// --read paced on 12 November: more threads read faster but slow the page model beside them, and on a Windows
// 11 laptop (Core i9-13900HX) only 2 kept the processor's per-frame p95 under a 30 fps frame (33 ms) while
// the reader was busy; the Mac (M4 Pro) stayed under it at every count tried.
inline constexpr int kReaderIntraOpThreads = 2;

// DocsaidLab MRZScanner 1.0.7 (Apache-2.0 weights), as the spike measured it: detection on the page padded to
// a square with black, 256 × 256 / 255, CHW, BGR as it is; the heatmap resized to the padded page, Otsu, the
// largest external contour's minAreaRect. Recognition of the band warped flat, 64 × 640 / 255; arg-max
// tokens.
class MrzScanner final : public MrzReader {
public:
    static std::unique_ptr<MrzScanner> open(const std::filesystem::path& detection,
                                            const std::filesystem::path& recognition, std::string& error,
                                            OpenFailure* failure = nullptr,
                                            int intraOpThreads = kReaderIntraOpThreads);
    MrzReading read(const cv::Mat& page, std::string& error) override;
    void cancel() override;

private:
    MrzScanner(std::unique_ptr<OnnxModel> detection, std::unique_ptr<OnnxModel> recognition);

    std::unique_ptr<OnnxModel> m_detection;
    std::unique_ptr<OnnxModel> m_recognition;
};

// The recogniser's output [1, steps, classes] as lines: 0 PAD skipped, 1 EOS ends, 2 SEP starts a new line,
// 3–28 A–Z, 29–38 0–9, 39 '<'. Empty for any other shape.
std::vector<std::string> decodeMrzTokens(const Tensor& output);

// Reads `page`, turned 180° first when `turnFirst`. When the band's centre is in the top third of the page as
// read, reads it the other way too, and returns that reading unless its band is also in the top third (then
// the first). `turned`: whether the returned reading is of the turned page. An error ends it with the first
// read's.
MrzReading readUpright(MrzReader& reader, const cv::Mat& page, bool turnFirst, bool& turned,
                       std::string& error);

} // namespace doc_vision
