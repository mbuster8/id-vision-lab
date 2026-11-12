// doc_vision_eval: the Document processor, as the Document Lab runs it, on labelled clips, on MIDV-2020's
// annotated passport clips, or on any replay without labels (a video, an image, a folder of images or a
// Document Lab recording), to tune and check A3.1's gates and timing. It prints aggregates only: no frame, no
// text from a page, and clips by their number.
//
//   doc_vision_eval --model <onnx> [options] --labels <labels.csv> <clip>...
//   doc_vision_eval --model <onnx> [options] --midv <clips folder> --frames <n> <type>...
//   doc_vision_eval --model <onnx> [options] --replay <video, image, folder or recording>...
//
// Options: --threads <n> (ONNX Runtime's intra-op threads; 0 for its default), --margin <share> (the in-view
// gate fails under it and passes again 0.005 above), --sharpness <value> (the Sharp gate fails under it and
// passes again 10% above), --size <width>x<height> (each frame resized first,
// INTER_LINEAR). labels.csv has rows "clip,frame,state[,x0,y0,...,x3,y3]": state "doc" (the page wholly in
// view, with its corners), "part" (cut by the frame's edge) or "none". MIDV-2020's frames are cut when a
// corner of their doc_quad lies outside the image.
//
// A4, reading the MRZ (with --replay or --midv; not --labels, whose loop reuses one cv::Mat for every frame):
//   --read every    every gate-passing frame read before the next is processed: deterministic, for accepted
//                   right / wrong / never, reads made, the share valid and valid-but-wrong reads by line;
//   --read paced    frames paced at their own times (frame_io::ReplayPacer), the reader on its own thread as
//                   in the lab: for the time to accept, each stage's timing, and the processor's per-frame
//                   time while the reader is busy;
//   --reader-threads <n> (the reader's intra-op threads; 0 for ONNX Runtime's default), --truth <file> (the
//   MRZ every replayed clip shows: its first two lines, 44 characters each once whitespace is removed;
//   compared inside, never printed), --midv-templates <folder> (MIDV-2020's templates: each clip's truth from
//   its template's mrz_line0 and mrz_line1; with --read, MIDV's clips are 00, 04, …, 96), --band-stats (p5,
//   p50 and p95 of the band's peak, width share and aspect over the reads), --negatives <folder> (pages with
//   no MRZ, read once each, for the same statistics). The MRZ models are looked for beside --model.

#include <doc_vision/guidance.h>
#include <doc_vision/mrz.h>
#include <doc_vision/mrz_reader.h>
#include <doc_vision/onnx_model.h>
#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>
#include <doc_vision/quality_gates.h>
#include <frame_io/image_folder.h>
#include <frame_io/recording.h>
#include <frame_io/replay_pacer.h>
#include <lab_core/document_processor.h>
#include <lab_core/focus_controller.h>
#include <lab_core/mrz_worker.h>
#include <lab_core/processors.h>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp> // OpenCV 5's home of convexHull, contourArea and intersectConvexConvex
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using doc_vision::Gate;
using doc_vision::Hint;
using doc_vision::kGateCount;
using doc_vision::Quad;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string percentiles(std::vector<double> values) {
    if (values.empty()) {
        return "n/a";
    }
    std::sort(values.begin(), values.end());
    const auto at = [&values](double q) {
        return values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1) + 0.5)];
    };
    char text[96];
    std::snprintf(text, sizeof text, "p50 %6.2f  p95 %6.2f  (%zu)", at(0.5), at(0.95), values.size());
    return text;
}

double share(int part, int whole) { return whole > 0 ? 100.0 * part / whole : 0.0; }

// Intersection over union of two quadrilaterals, whatever their corners' order.
double quadIoU(const Quad& a, const Quad& b) {
    std::vector<cv::Point2f> pa(a.begin(), a.end()), pb(b.begin(), b.end()), ha, hb, overlap;
    cv::convexHull(pa, ha);
    cv::convexHull(pb, hb);
    const double inter = cv::intersectConvexConvex(ha, hb, overlap, true);
    const double unionArea = cv::contourArea(ha) + cv::contourArea(hb) - inter;
    return unionArea > 0 ? inter / unionArea : 0.0;
}

struct Timings {
    std::vector<double> convert, model, post, warp, gates, total;
};

// The detector as PageDetector::find runs it, with its two stages timed and its raw quad kept for scoring.
class TimedFinder {
public:
    TimedFinder(doc_vision::PageDetector& detector, Timings& timings)
        : m_detector(detector), m_timings(timings) {}

    std::optional<doc_vision::PageFound> find(const cv::Mat& bgr8, std::string& error) {
        raw.reset();
        const Clock::time_point modelStart = Clock::now();
        const std::vector<cv::Mat> maps = m_detector.heatmaps(bgr8, error);
        m_timings.model.push_back(msSince(modelStart));
        if (maps.size() != 4) {
            return std::nullopt;
        }
        const Clock::time_point postStart = Clock::now();
        std::optional<doc_vision::PageFound> found;
        if (const auto corners = doc_vision::cornersFromHeatmaps(maps, bgr8.size())) {
            double weakest = 1.0;
            for (const cv::Mat& map : maps) {
                double peak = 0;
                cv::minMaxLoc(map, nullptr, &peak);
                weakest = std::min(weakest, peak);
            }
            found = doc_vision::PageFound{doc_vision::orderedCorners(*corners), weakest};
            raw = found->quad;
        }
        m_timings.post.push_back(msSince(postStart));
        return found;
    }

    std::optional<Quad> raw; // the last frame's, before the processor's hold and alignment

private:
    doc_vision::PageDetector& m_detector;
    Timings& m_timings;
};

// The processor owns its finder; this one hands each call to the evaluation's.
class Forwarding final : public doc_vision::PageFinder {
public:
    explicit Forwarding(TimedFinder& finder) : m_finder(finder) {}
    std::optional<doc_vision::PageFound> find(const cv::Mat& bgr8, std::string& error) override {
        return m_finder.find(bgr8, error);
    }

private:
    TimedFinder& m_finder;
};

// A4: every read the processor's reader makes, from whichever thread reads.
struct ReadLog {
    std::mutex mutex;
    std::vector<doc_vision::MrzReading> readings; // this clip's, error-free
    int errors = 0;
    bool ending = false; // the clip's processor is going: a read it cancels is no error
    std::atomic<bool> busy{false};
    std::vector<double> openMs;
};

class LoggingReader final : public doc_vision::MrzReader {
public:
    LoggingReader(std::unique_ptr<doc_vision::MrzReader> inner, ReadLog& log)
        : m_inner(std::move(inner)), m_log(log) {}
    doc_vision::MrzReading read(const cv::Mat& page, std::string& error) override {
        m_log.busy = true;
        doc_vision::MrzReading reading = m_inner->read(page, error);
        {
            const std::lock_guard lock(m_log.mutex);
            if (error.empty()) {
                m_log.readings.push_back(reading);
            } else if (!m_log.ending) {
                ++m_log.errors;
            }
        }
        m_log.busy = false;
        return reading;
    }
    void cancel() override { m_inner->cancel(); }

private:
    std::unique_ptr<doc_vision::MrzReader> m_inner;
    ReadLog& m_log;
};

// What reading one clip came to. Nothing of what was read: only whether it was right.
struct ClipReads {
    enum class Accept { Never, Right, Wrong, Unknown };
    Accept accept = Accept::Never;
    std::optional<double> acceptedAt; // the clip's time, s
    std::optional<double> timeToScan; // the scan's own, s
    int reads = 0;
    int valid = 0;
    int validWrongLine1 = 0; // valid reads (every check digit passes) that are not the truth, by line
    int validWrongLine2 = 0;
    int errors = 0;
};

// What one group of frames (a label, or MIDV's in-view and cut frames) came to.
struct Group {
    int frames = 0;
    int found = 0; // by the raw detector
    int iou90 = 0; // of frames with a true quad
    int withTruth = 0;
    std::array<int, kGateCount> passed{}; // of found frames, judged afresh (no hysteresis)
    int allPassed = 0;
    int frameGood = 0;
    int shownGood = 0;
    std::vector<double> sharpness; // of the pages found wholly in view (the In view gate passed)
};

struct Run {
    std::map<std::string, Group> groups;
    Timings timings;
    int switches = 0;
    double seconds = 0;
    std::vector<std::optional<double>> firstGood; // per clip
    // A3.3's rule for a camera the lab cannot focus, run on the replay: how often it would say so, and the
    // first time how long after sharpness began to fail first.
    int cannotFocus = 0;
    std::optional<double> firstCannotFocusAfter;
    // A4's reading.
    std::vector<ClipReads> reads;
    std::vector<double> detect, recognise, busyFrame; // ms
    std::vector<double> peak, widthShare, aspect;     // the reads' band candidates
};

struct Options {
    std::filesystem::path model;
    int threads = doc_vision::kDefaultIntraOpThreads;
    std::optional<double> margin;
    std::optional<double> sharpness;
    std::optional<cv::Size> size;
    std::optional<std::filesystem::path> labels;
    std::optional<std::filesystem::path> midv;
    bool replay = false;
    int frames = 30;
    std::vector<std::string> inputs;
    // A4.
    std::optional<std::string> read; // "every" or "paced"
    int readerThreads = doc_vision::kReaderIntraOpThreads;
    std::optional<std::filesystem::path> truth;
    std::optional<std::filesystem::path> midvTemplates;
    bool bandStats = false;
    std::optional<std::filesystem::path> negatives;
};

// The MRZ as compared: whitespace removed, upper-cased, two lines of 44 joined by '\n' (Mrz::text()'s form).
std::optional<std::string> truthOf(const std::string& line1, const std::string& line2) {
    const auto clean = [](const std::string& line) {
        std::string out;
        for (const char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        return out;
    };
    const std::string first = clean(line1);
    const std::string second = clean(line2);
    if (first.size() != 44 || second.size() != 44) {
        return std::nullopt;
    }
    return first + "\n" + second;
}

// The owner's truth file: its first two lines. Never printed.
std::optional<std::string> readTruthFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line1;
    std::string line2;
    if (!std::getline(in, line1) || !std::getline(in, line2)) {
        return std::nullopt;
    }
    return truthOf(line1, line2);
}

// MIDV-2020's template for clip `clip` of `type`: its mrz_line0 and mrz_line1.
std::optional<std::string> midvTruth(const std::filesystem::path& templates, const std::string& type,
                                     const std::string& clip) {
    std::ifstream in(templates / "annotations" / (type + ".json"));
    if (!in) {
        return std::nullopt;
    }
    const nlohmann::json annotations = nlohmann::json::parse(in, nullptr, false);
    if (annotations.is_discarded() || !annotations.contains("_via_img_metadata")) {
        return std::nullopt;
    }
    for (const auto& [key, meta] : annotations["_via_img_metadata"].items()) {
        if (meta.value("filename", "") != clip + ".jpg") {
            continue;
        }
        std::string lines[2];
        for (const auto& region : meta["regions"]) {
            const std::string field = region["region_attributes"].value("field_name", "");
            if (field == "mrz_line0" || field == "mrz_line1") {
                lines[field == "mrz_line0" ? 0 : 1] = region["region_attributes"].value("value", "");
            }
        }
        return truthOf(lines[0], lines[1]);
    }
    return std::nullopt;
}

std::unique_ptr<doc_vision::MrzScanner> openScanner(const Options& options, std::string& error,
                                                    doc_vision::OpenFailure* failure = nullptr) {
    const std::filesystem::path folder = options.model.parent_path();
    return doc_vision::MrzScanner::open(folder / lab_core::kMrzDetectionModelFile,
                                        folder / lab_core::kMrzRecognitionModelFile, error, failure,
                                        options.readerThreads);
}

doc_vision::Thresholds thresholdsOf(const Options& options) {
    doc_vision::Thresholds thresholds;
    if (options.margin) {
        thresholds.edgeMargin = {*options.margin, *options.margin + 0.005};
    }
    if (options.sharpness) {
        thresholds.sharpness = {*options.sharpness, *options.sharpness * 1.1};
    }
    return thresholds;
}

// One frame through the processor, scored into `group` when it has one.
class Scorer {
public:
    Scorer(const Options& options, doc_vision::PageDetector& detector, Run& run)
        : m_options(options), m_thresholds(thresholdsOf(options)), m_finder(detector, run.timings),
          m_run(run) {}

    // `truth`: the MRZ this clip shows, when known (A4's --read).
    void startClip(std::optional<std::string> truth = std::nullopt) {
        lab_core::DocumentProcessor::Reader reader;
        if (m_options.read) {
            reader.mode = *m_options.read == "every" ? lab_core::MrzWorker::Mode::Manual
                                                     : lab_core::MrzWorker::Mode::Threaded;
            reader.make = [this](std::string& error,
                                 doc_vision::OpenFailure& failure) -> std::unique_ptr<doc_vision::MrzReader> {
                const Clock::time_point start = Clock::now();
                std::unique_ptr<doc_vision::MrzScanner> scanner = openScanner(m_options, error, &failure);
                {
                    const std::lock_guard lock(m_log.mutex);
                    m_log.openMs.push_back(msSince(start));
                }
                if (!scanner) {
                    return nullptr;
                }
                return std::make_unique<LoggingReader>(std::move(scanner), m_log);
            };
            m_run.reads.emplace_back();
            m_truth = std::move(truth);
            // Compared as the parser writes a read: an empty personal number's check digit as '<', say.
            if (m_truth) {
                const std::string line1 = m_truth->substr(0, 44);
                const std::string line2 = m_truth->substr(45);
                if (const std::optional<doc_vision::Mrz> parsed = doc_vision::parseMrz({line1, line2})) {
                    m_truth = parsed->text();
                }
            }
            m_pacer.restart();
            const std::lock_guard lock(m_log.mutex);
            m_log.ending = false;
        }
        m_processor = std::make_unique<lab_core::DocumentProcessor>(
            [this](std::string&, doc_vision::OpenFailure&) -> std::unique_ptr<doc_vision::PageFinder> {
                return std::make_unique<Forwarding>(m_finder);
            },
            m_thresholds, std::move(reader));
        m_shown.reset();
        m_run.firstGood.emplace_back();
        // No control: the rule for a camera the lab cannot focus. Every frame of a replay counts at once.
        m_focus = std::make_unique<lab_core::FocusController>(nullptr, [this] { return m_number + 1; });
        m_focusState = lab_core::FocusStatus::State::Camera;
    }

    // Whether the frame brought an accept (with --read: the clip can end there).
    bool frame(cv::Mat image, std::uint64_t number, double seconds, Group* group,
               const std::optional<Quad>& truth, const visual_debug::ImageDescription& tags = {}) {
        if (m_options.size && image.size() != *m_options.size) {
            cv::resize(image, image, *m_options.size, 0, 0, cv::INTER_LINEAR);
        }
        const frame_io::Frame frame{.image = image,
                                    .number = number,
                                    .at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::duration<double>(seconds)),
                                    .tags = tags};
        const bool paced = m_options.read && *m_options.read == "paced";
        if (paced) {
            std::this_thread::sleep_until(m_pacer.due(frame.at, Clock::now()));
        }
        const bool readerBusy = m_log.busy;
        const Clock::time_point start = Clock::now();
        const lab_core::ProcessorResult result = m_processor->process(frame);
        const double took = msSince(start);
        m_run.timings.total.push_back(took);
        if (paced && readerBusy) {
            m_run.busyFrame.push_back(took);
        }
        if (m_options.read && !paced) {
            m_processor->pumpReaderForTests(); // read now: the next frame collects it
        }
        const Clock::time_point convertStart = Clock::now();
        const cv::Mat bgr = doc_vision::toBgr8(image, tags.valueRange.min, tags.valueRange.max);
        m_run.timings.convert.push_back(msSince(convertStart));

        if (!result.guidance) {
            return false;
        }
        bool accepted = false;
        if (m_options.read && result.guidance->scan &&
            m_run.reads.back().accept == ClipReads::Accept::Never) {
            ClipReads& clip = m_run.reads.back();
            const std::string text = result.guidance->scan->mrz.text();
            clip.accept = !m_truth           ? ClipReads::Accept::Unknown
                          : text == *m_truth ? ClipReads::Accept::Right
                                             : ClipReads::Accept::Wrong;
            clip.acceptedAt = seconds;
            clip.timeToScan = std::chrono::duration<double>(result.guidance->scan->timeToScan).count();
            accepted = true;
        }
        m_number = number;
        const lab_core::FocusStatus focus = m_focus->next(frame, result.guidance->page);
        if (focus.state == lab_core::FocusStatus::State::CannotFocus && m_focusState != focus.state) {
            ++m_run.cannotFocus;
            if (!m_run.firstCannotFocusAfter && focus.blurredFirstSince) {
                m_run.firstCannotFocusAfter =
                    std::chrono::duration<double>(frame.at - *focus.blurredFirstSince).count();
            }
        }
        m_focusState = focus.state;
        const Hint shown = result.guidance->shownHint;
        if (m_shown && *m_shown != shown) {
            ++m_run.switches;
        }
        m_shown = shown;
        // A4's hints 11 and 12 also mean every gate passes: A3.1's counts keep their meaning.
        if (doc_vision::gatesPass(shown) && !m_run.firstGood.back()) {
            m_run.firstGood.back() = seconds;
        }
        std::optional<doc_vision::Verdicts> verdicts;
        double sharpness = 0;
        if (m_finder.raw) {
            const Clock::time_point warpStart = Clock::now();
            const cv::Mat page = doc_vision::flatten(bgr, *m_finder.raw, doc_vision::kGatePageSize);
            const double warp = msSince(warpStart);
            const Clock::time_point measureStart = Clock::now();
            const doc_vision::Measures measures = doc_vision::measure(bgr, *m_finder.raw);
            m_run.timings.warp.push_back(warp);
            m_run.timings.gates.push_back(std::max(0.0, msSince(measureStart) - warp));
            verdicts = doc_vision::GateJudge(m_thresholds).judge(measures);
            sharpness = measures.sharpness;
        }
        if (group == nullptr) {
            return accepted;
        }
        ++group->frames;
        if (m_finder.raw) {
            ++group->found;
            for (std::size_t i = 0; i < kGateCount; ++i) {
                group->passed[i] += verdicts->passed[i] ? 1 : 0;
            }
            group->allPassed += verdicts->allPassed() ? 1 : 0;
            if (verdicts->passed[static_cast<std::size_t>(Gate::InView)]) {
                group->sharpness.push_back(sharpness);
            }
        }
        if (truth) {
            ++group->withTruth;
            group->iou90 += m_finder.raw && quadIoU(*truth, *m_finder.raw) > 0.9 ? 1 : 0;
        }
        group->frameGood += doc_vision::gatesPass(result.guidance->frameHint) ? 1 : 0;
        group->shownGood += doc_vision::gatesPass(shown) ? 1 : 0;
        return accepted;
    }

    // The clip's reads, once its processor (and a read in progress) is gone: counted, scored against the
    // truth, timed. Nothing read is kept.
    void finishClip() {
        {
            const std::lock_guard lock(m_log.mutex);
            m_log.ending = true;
        }
        m_processor.reset(); // cancels and joins a threaded reader
        if (!m_options.read) {
            return;
        }
        std::vector<doc_vision::MrzReading> readings;
        {
            const std::lock_guard lock(m_log.mutex);
            readings.swap(m_log.readings);
            m_run.reads.back().errors = m_log.errors;
            m_log.errors = 0;
            m_log.ending = false;
        }
        ClipReads& clip = m_run.reads.back();
        for (const doc_vision::MrzReading& reading : readings) {
            ++clip.reads;
            m_run.detect.push_back(std::chrono::duration<double, std::milli>(reading.detectTime).count());
            if (reading.band) {
                m_run.recognise.push_back(
                    std::chrono::duration<double, std::milli>(reading.recogniseTime).count());
            }
            m_run.peak.push_back(reading.peak);
            m_run.widthShare.push_back(reading.widthShare);
            m_run.aspect.push_back(reading.aspect);
            const std::optional<doc_vision::Mrz> mrz = doc_vision::parseMrz(reading.lines);
            if (!mrz || !mrz->valid()) {
                continue;
            }
            ++clip.valid;
            if (m_truth && mrz->text() != *m_truth && mrz->lines.size() == 2) {
                const std::string truthLine1 = m_truth->substr(0, 44);
                const std::string truthLine2 = m_truth->substr(45);
                clip.validWrongLine1 += mrz->lines[0] != truthLine1 ? 1 : 0;
                clip.validWrongLine2 += mrz->lines[1] != truthLine2 ? 1 : 0;
            }
        }
    }

    ReadLog& log() { return m_log; }

private:
    const Options& m_options;
    doc_vision::Thresholds m_thresholds;
    TimedFinder m_finder;
    Run& m_run;
    ReadLog m_log;
    std::optional<std::string> m_truth;
    frame_io::ReplayPacer m_pacer;
    std::unique_ptr<lab_core::DocumentProcessor> m_processor;
    std::optional<Hint> m_shown;
    std::unique_ptr<lab_core::FocusController> m_focus;
    std::uint64_t m_number = 0;
    lab_core::FocusStatus::State m_focusState = lab_core::FocusStatus::State::Camera;
};

struct Label {
    std::string state;
    std::optional<Quad> quad;
};

std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// How labels and clips are matched: the file name up to any ':'. A clip copied on the Mac can carry a
// ":Users:…" tail that a Windows file name cannot hold, so the same clip has its plain name there.
std::string clipKey(const std::string& path) {
    const std::string name = fileNameOf(path);
    return name.substr(0, name.find(':'));
}

// clip file name → frame → label.
std::map<std::string, std::map<int, Label>> readLabels(const std::filesystem::path& path) {
    std::map<std::string, std::map<int, Label>> labels;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::stringstream row(line);
        std::string field;
        while (std::getline(row, field, ',')) {
            fields.push_back(field);
        }
        if (fields.size() < 3) {
            continue;
        }
        Label label{.state = fields[2], .quad = std::nullopt};
        if (label.state == "doc" && fields.size() >= 11) {
            Quad quad{};
            for (std::size_t i = 0; i < 4; ++i) {
                quad[i] = {std::stod(fields[3 + 2 * i]), std::stod(fields[4 + 2 * i])};
            }
            label.quad = quad;
        }
        labels[clipKey(fields[0])][std::stoi(fields[1])] = label;
    }
    return labels;
}

Quad scaled(const Quad& quad, double sx, double sy) {
    Quad out = quad;
    for (cv::Point2d& corner : out) {
        corner = {(corner.x + 0.5) * sx - 0.5, (corner.y + 0.5) * sy - 0.5};
    }
    return out;
}

void runLabelled(const Options& options, Scorer& scorer, Run& run) {
    const auto labels = readLabels(*options.labels);
    for (std::size_t c = 0; c < options.inputs.size(); ++c) {
        cv::VideoCapture capture(options.inputs[c]);
        if (!capture.isOpened()) {
            std::fprintf(stderr, "clip %zu: cannot be opened\n", c + 1);
            continue;
        }
        double fps = capture.get(cv::CAP_PROP_FPS);
        if (!(fps > 1.0 && fps < 1000.0)) {
            fps = 30.0;
        }
        const auto clipLabels = labels.find(clipKey(options.inputs[c]));
        scorer.startClip();
        cv::Mat image;
        int i = 0;
        for (; capture.read(image); ++i) {
            Group* group = nullptr;
            std::optional<Quad> truth;
            if (clipLabels != labels.end()) {
                if (const auto label = clipLabels->second.find(i); label != clipLabels->second.end()) {
                    group = &run.groups[label->second.state];
                    if (label->second.quad) {
                        truth = options.size ? scaled(*label->second.quad,
                                                      static_cast<double>(options.size->width) / image.cols,
                                                      static_cast<double>(options.size->height) / image.rows)
                                             : *label->second.quad;
                    }
                }
            }
            scorer.frame(image, static_cast<std::uint64_t>(i), i / fps, group, truth);
        }
        scorer.finishClip();
        run.seconds += i / fps;
    }
}

// Any replay the Document Lab opens, without labels: every frame at its own time, its tags too (a 16-bit
// recording's value range), into one group.
void runReplay(const Options& options, Scorer& scorer, Run& run) {
    std::optional<std::string> truth;
    if (options.read && options.truth) {
        truth = readTruthFile(*options.truth);
        if (!truth) {
            std::printf("truth file not used: right and wrong are unknown\n");
        }
    }
    for (std::size_t c = 0; c < options.inputs.size(); ++c) {
        std::string error;
        const std::unique_ptr<frame_io::ReplaySource> source =
            frame_io::openReplay(std::filesystem::path(options.inputs[c]), error);
        if (!source) {
            std::fprintf(stderr, "source %zu: cannot be opened: %s\n", c + 1, error.c_str());
            continue;
        }
        scorer.startClip(truth);
        double last = 0;
        for (;;) {
            frame_io::ReadResult read = source->next();
            if (read.status != frame_io::ReadStatus::Frame) {
                if (read.status == frame_io::ReadStatus::Error) {
                    std::fprintf(stderr, "source %zu: %s\n", c + 1, read.error.c_str());
                }
                break;
            }
            last = std::chrono::duration<double>(read.frame.at).count();
            if (scorer.frame(read.frame.image, read.frame.number, last, &run.groups["all"], std::nullopt,
                             read.frame.tags)) {
                break; // accepted: the clip's reading is done
            }
        }
        scorer.finishClip();
        run.seconds += last;
    }
}

// MIDV-2020's annotated frames of a clip, in order, with each frame's number from its file name.
struct Annotated {
    std::filesystem::path file;
    int number = 0;
    Quad quad{};
};

std::vector<Annotated> annotatedFrames(const std::filesystem::path& clips, const std::string& type,
                                       const std::string& clip) {
    std::ifstream in(clips / "annotations" / type / (clip + ".json"));
    if (!in) {
        return {};
    }
    const nlohmann::json annotations = nlohmann::json::parse(in, nullptr, false);
    if (annotations.is_discarded() || !annotations.contains("_via_img_metadata")) {
        return {};
    }
    std::vector<Annotated> frames;
    for (const auto& [key, meta] : annotations["_via_img_metadata"].items()) {
        for (const auto& region : meta["regions"]) {
            if (region["region_attributes"].value("field_name", "") != "doc_quad") {
                continue;
            }
            const auto& xs = region["shape_attributes"]["all_points_x"];
            const auto& ys = region["shape_attributes"]["all_points_y"];
            if (xs.size() != 4 || ys.size() != 4) {
                continue;
            }
            Annotated frame;
            const std::string name = meta["filename"].get<std::string>();
            frame.file = clips / "images" / type / clip / name;
            frame.number = std::stoi(name.substr(0, name.find('.')));
            for (std::size_t i = 0; i < 4; ++i) {
                frame.quad[i] = {xs[i].get<double>(), ys[i].get<double>()};
            }
            frames.push_back(frame);
        }
    }
    std::sort(frames.begin(), frames.end(),
              [](const Annotated& a, const Annotated& b) { return a.number < b.number; });
    return frames;
}

void runMidv(const Options& options, Scorer& scorer, Run& run) {
    int clipsSeen = 0;
    for (const std::string& type : options.inputs) {
        // Reading: clips 00, 04, …, 96, as the spike ran them (spec §6).
        for (int c = 0; c < 100; c += options.read ? 4 : 1) {
            char clip[8];
            std::snprintf(clip, sizeof clip, "%02d", c);
            std::vector<Annotated> frames = annotatedFrames(*options.midv, type, clip);
            if (frames.empty()) {
                continue;
            }
            ++clipsSeen;
            if (static_cast<int>(frames.size()) > options.frames) {
                frames.resize(static_cast<std::size_t>(options.frames));
            }
            scorer.startClip(options.read && options.midvTemplates
                                 ? midvTruth(*options.midvTemplates, type, clip)
                                 : std::nullopt);
            const int firstNumber = frames.front().number;
            for (const Annotated& frame : frames) {
                const cv::Mat image = cv::imread(frame.file.string(), cv::IMREAD_COLOR);
                if (image.empty()) {
                    continue;
                }
                const Quad truth =
                    options.size ? scaled(frame.quad, static_cast<double>(options.size->width) / image.cols,
                                          static_cast<double>(options.size->height) / image.rows)
                                 : frame.quad;
                const cv::Size size = options.size.value_or(image.size());
                const bool cut = doc_vision::edgeMarginShare(truth, size) < 0;
                if (scorer.frame(image, static_cast<std::uint64_t>(frame.number),
                                 (frame.number - firstNumber) / 30.0, &run.groups[cut ? "cut" : "in view"],
                                 cut ? std::nullopt : std::optional<Quad>(truth))) {
                    break; // accepted
                }
            }
            scorer.finishClip();
            run.seconds += (frames.back().number - firstNumber) / 30.0;
        }
    }
    std::printf(
        "MIDV-2020: %d clips, up to %d annotated frames each, frames 0.2 s apart (every 6th at 30 fps)\n",
        clipsSeen, options.frames);
}

void report(const Run& run) {
    constexpr std::array kGateNames{"In view", "Size", "Flat", "Sharp", "No glare", "Light"};
    std::printf("\n%-8s %6s %7s %8s", "group", "frames", "found", "IoU>0.9");
    for (const char* name : kGateNames) {
        std::printf(" %8s", name);
    }
    std::printf(" %8s %10s %10s\n", "all", "frame Good", "shown Good");
    for (const auto& [name, g] : run.groups) {
        std::printf("%-8s %6d %6.1f%% ", name.c_str(), g.frames, share(g.found, g.frames));
        if (g.withTruth > 0) {
            std::printf("%7.1f%%", share(g.iou90, g.withTruth));
        } else {
            std::printf("%8s", "-");
        }
        for (std::size_t i = 0; i < kGateCount; ++i) {
            std::printf(" %7.1f%%", share(g.passed[i], g.found));
        }
        std::printf(" %7.1f%% %5.1f%% (%d) %5.1f%% (%d)\n", share(g.allPassed, g.found),
                    share(g.frameGood, g.frames), g.frameGood, share(g.shownGood, g.frames), g.shownGood);
    }
    std::printf(
        "(gates: share of the frames the raw detector found, each judged afresh; Good: share of all)\n");
    std::printf("\nsharpness of the pages found wholly in view (the Sharp gate fails under %.0f):\n",
                doc_vision::Thresholds{}.sharpness.failsAt);
    for (const auto& [name, g] : run.groups) {
        std::vector<double> values = g.sharpness;
        std::sort(values.begin(), values.end());
        if (values.empty()) {
            std::printf("  %-8s none\n", name.c_str());
            continue;
        }
        const auto at = [&values](double q) {
            return values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1) + 0.5)];
        };
        std::printf("  %-8s %zu pages: p5 %.0f, p25 %.0f, p50 %.0f, p75 %.0f, p95 %.0f; at or above",
                    name.c_str(), values.size(), at(0.05), at(0.25), at(0.5), at(0.75), at(0.95));
        for (const double threshold : {80.0, 100.0, 120.0, 150.0, 200.0}) {
            const auto above = values.end() - std::lower_bound(values.begin(), values.end(), threshold);
            std::printf(" %.0f: %.1f%%%s", threshold, 100.0 * static_cast<double>(above) / values.size(),
                        threshold < 200.0 ? "," : "\n");
        }
    }
    std::printf("\nbanner switches a minute: %.1f (over %.1f s)\n",
                run.seconds > 0 ? 60.0 * run.switches / run.seconds : 0.0, run.seconds);
    if (run.firstCannotFocusAfter) {
        std::printf(
            "can't focus (no control): %d episodes, the first %.2f s after sharpness began to fail first\n",
            run.cannotFocus, *run.firstCannotFocusAfter);
    } else {
        std::printf("can't focus (no control): %d episodes\n", run.cannotFocus);
    }
    if (run.firstGood.size() <= 12) {
        std::printf("first Good:");
        for (std::size_t c = 0; c < run.firstGood.size(); ++c) {
            if (run.firstGood[c]) {
                std::printf(" clip %zu %.2f s;", c + 1, *run.firstGood[c]);
            } else {
                std::printf(" clip %zu never;", c + 1);
            }
        }
        std::printf("\n");
    }
    std::printf("\nms per frame:\n");
    std::printf("  process (all of it) %s\n", percentiles(run.timings.total).c_str());
    std::printf("  convert             %s\n", percentiles(run.timings.convert).c_str());
    std::printf("  model               %s\n", percentiles(run.timings.model).c_str());
    std::printf("  post                %s\n", percentiles(run.timings.post).c_str());
    std::printf("  warp                %s\n", percentiles(run.timings.warp).c_str());
    std::printf("  gates               %s\n", percentiles(run.timings.gates).c_str());
}

std::string spread(std::vector<double> values) {
    if (values.empty()) {
        return "n/a";
    }
    std::sort(values.begin(), values.end());
    const auto at = [&values](double q) {
        return values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1) + 0.5)];
    };
    char text[96];
    std::snprintf(text, sizeof text, "p5 %.3f  p50 %.3f  p95 %.3f  (%zu)", at(0.05), at(0.5), at(0.95),
                  values.size());
    return text;
}

struct BandStats {
    std::vector<double> peak, widthShare, aspect;
};

// How many candidates pass as a band (peak, width share and aspect all at or above their limits) at each of a
// few width-share limits, the others at doc_vision's: the joint count the limits are set by.
std::string bandPasses(const std::vector<double>& peak, const std::vector<double>& widthShare,
                       const std::vector<double>& aspect) {
    std::string out;
    for (const double limit : {0.60, 0.70, 0.75, 0.80, 0.85}) {
        int passes = 0;
        for (std::size_t i = 0; i < peak.size(); ++i) {
            passes += peak[i] >= doc_vision::kBandPeak && widthShare[i] >= limit &&
                              aspect[i] >= doc_vision::kBandAspect
                          ? 1
                          : 0;
        }
        char text[48];
        std::snprintf(text, sizeof text, "%s≥%.2f: %d", out.empty() ? "" : ", ", limit, passes);
        out += text;
    }
    return out + " (of " + std::to_string(peak.size()) + ")";
}

// MIDV-2020's photo annotations for a folder of its photos (photo/images/<type>): file name → doc_quad.
std::map<std::string, Quad> photoQuads(const std::filesystem::path& folder) {
    std::map<std::string, Quad> quads;
    std::ifstream in(folder.parent_path().parent_path() / "annotations" /
                     (folder.filename().string() + ".json"));
    if (!in) {
        return quads;
    }
    const nlohmann::json annotations = nlohmann::json::parse(in, nullptr, false);
    if (annotations.is_discarded() || !annotations.contains("_via_img_metadata")) {
        return quads;
    }
    for (const auto& [key, meta] : annotations["_via_img_metadata"].items()) {
        for (const auto& region : meta["regions"]) {
            if (region["region_attributes"].value("field_name", "") != "doc_quad") {
                continue;
            }
            const auto& xs = region["shape_attributes"]["all_points_x"];
            const auto& ys = region["shape_attributes"]["all_points_y"];
            if (xs.size() != 4 || ys.size() != 4) {
                continue;
            }
            std::array<cv::Point2d, 4> points{};
            for (std::size_t i = 0; i < 4; ++i) {
                points[i] = {xs[i].get<double>(), ys[i].get<double>()};
            }
            quads[meta.value("filename", "")] = doc_vision::orderedCorners(points);
        }
    }
    return quads;
}

// Pages with no MRZ (an ID card's front, say), each read once: where a band's candidate falls when there is
// none. A MIDV-2020 photo is flattened by its annotated doc_quad to the page's size, as the lab flattens the
// page it finds; any other image is taken as a page and resized.
BandStats readNegatives(const Options& options) {
    BandStats stats;
    std::string error;
    const std::unique_ptr<doc_vision::MrzScanner> scanner = openScanner(options, error);
    if (!scanner) {
        std::fprintf(stderr, "the MRZ reader could not be opened: %s\n", error.c_str());
        return stats;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(*options.negatives)) {
        if (entry.is_regular_file() && frame_io::isImageFileName(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    const std::map<std::string, Quad> quads = photoQuads(*options.negatives);
    for (const std::filesystem::path& file : files) {
        const cv::Mat image = cv::imread(file.string(), cv::IMREAD_COLOR);
        if (image.empty()) {
            continue;
        }
        cv::Mat page;
        if (const auto quad = quads.find(file.filename().string()); quad != quads.end()) {
            page = doc_vision::flatten(image, quad->second, doc_vision::kPageSize);
        } else {
            cv::resize(image, page, doc_vision::kPageSize, 0, 0, cv::INTER_AREA);
        }
        error.clear();
        const doc_vision::MrzReading reading = scanner->read(page, error);
        if (!error.empty()) {
            continue;
        }
        stats.peak.push_back(reading.peak);
        stats.widthShare.push_back(reading.widthShare);
        stats.aspect.push_back(reading.aspect);
    }
    return stats;
}

const char* acceptWord(ClipReads::Accept accept) {
    switch (accept) {
    case ClipReads::Accept::Never:
        return "never";
    case ClipReads::Accept::Right:
        return "right";
    case ClipReads::Accept::Wrong:
        return "WRONG";
    case ClipReads::Accept::Unknown:
        return "accepted (truth unknown)";
    }
    return "?";
}

// Aggregates only: whether each clip's accept was right, never what was read.
void reportReading(const Options& options, const Run& run, const ReadLog& log) {
    const bool paced = *options.read == "paced";
    std::printf("\nMRZ reading · %s · reader threads %d · band: peak %.2f, width share %.2f, aspect %.1f\n",
                paced ? "paced" : "every gate-passing frame", options.readerThreads, doc_vision::kBandPeak,
                doc_vision::kBandWidthShare, doc_vision::kBandAspect);
    int right = 0;
    int wrong = 0;
    int never = 0;
    int unknown = 0;
    int reads = 0;
    int valid = 0;
    int wrong1 = 0;
    int wrong2 = 0;
    int errors = 0;
    std::vector<double> toAccept;
    std::vector<double> toScan;
    const bool perClip = run.reads.size() <= 12;
    for (std::size_t c = 0; c < run.reads.size(); ++c) {
        const ClipReads& clip = run.reads[c];
        right += clip.accept == ClipReads::Accept::Right ? 1 : 0;
        wrong += clip.accept == ClipReads::Accept::Wrong ? 1 : 0;
        never += clip.accept == ClipReads::Accept::Never ? 1 : 0;
        unknown += clip.accept == ClipReads::Accept::Unknown ? 1 : 0;
        reads += clip.reads;
        valid += clip.valid;
        wrong1 += clip.validWrongLine1;
        wrong2 += clip.validWrongLine2;
        errors += clip.errors;
        if (clip.acceptedAt) {
            toAccept.push_back(*clip.acceptedAt);
            toScan.push_back(clip.timeToScan.value_or(0.0));
        }
        if (perClip) {
            std::printf("  clip %zu: %s", c + 1, acceptWord(clip.accept));
            if (clip.acceptedAt) {
                std::printf(" at %.2f s (time to scan %.2f s)", *clip.acceptedAt,
                            clip.timeToScan.value_or(0.0));
            }
            std::printf("; reads %d, valid %.1f%%, valid but wrong: line 1 %d, line 2 %d", clip.reads,
                        share(clip.valid, clip.reads), clip.validWrongLine1, clip.validWrongLine2);
            if (clip.errors > 0) {
                std::printf(", errors %d", clip.errors);
            }
            std::printf("\n");
        }
    }
    std::printf("accepted: right %d, wrong %d, never %d", right, wrong, never);
    if (unknown > 0) {
        std::printf(", truth unknown %d", unknown);
    }
    std::printf(" (of %zu clips)\n", run.reads.size());
    std::printf("reads %d, valid %.1f%%, valid but wrong: line 1 %d, line 2 %d, errors %d\n", reads,
                share(valid, reads), wrong1, wrong2, errors);
    std::printf("ms: reader open %s\n", percentiles(log.openMs).c_str());
    std::printf("ms: detection %s\n", percentiles(run.detect).c_str());
    std::printf("ms: recognition %s\n", percentiles(run.recognise).c_str());
    if (paced) {
        std::printf("ms: the processor per frame with the reader busy %s\n",
                    percentiles(run.busyFrame).c_str());
        if (!toAccept.empty()) {
            std::printf("s to accept (the clip's time): %s, worst %.2f\n", percentiles(toAccept).c_str(),
                        *std::max_element(toAccept.begin(), toAccept.end()));
            std::printf("s to scan (the scan's own): %s, worst %.2f\n", percentiles(toScan).c_str(),
                        *std::max_element(toScan.begin(), toScan.end()));
        }
    }
    if (options.bandStats) {
        std::printf("band candidates of the reads:\n  peak        %s\n  width share %s\n  aspect      %s\n",
                    spread(run.peak).c_str(), spread(run.widthShare).c_str(), spread(run.aspect).c_str());
        std::printf("  pass as a band, width share %s\n",
                    bandPasses(run.peak, run.widthShare, run.aspect).c_str());
    }
    if (options.negatives) {
        const BandStats negatives = readNegatives(options);
        std::printf(
            "band candidates of the negatives:\n  peak        %s\n  width share %s\n  aspect      %s\n",
            spread(negatives.peak).c_str(), spread(negatives.widthShare).c_str(),
            spread(negatives.aspect).c_str());
        std::printf("  pass as a band, width share %s\n",
                    bandPasses(negatives.peak, negatives.widthShare, negatives.aspect).c_str());
    }
}

int usage() {
    std::fprintf(
        stderr,
        "usage: doc_vision_eval --model <onnx> [--threads <n>] [--margin <share>] [--sharpness <v>] "
        "[--size <w>x<h>]\n"
        "                       --labels <labels.csv> <clip>...\n"
        "       doc_vision_eval --model <onnx> [--threads <n>] [--margin <share>] [--sharpness <v>] "
        "[--size <w>x<h>]\n"
        "                       --midv <clips folder> [--frames <n>] <type>...\n"
        "       doc_vision_eval --model <onnx> [--threads <n>] [--margin <share>] [--sharpness <v>] "
        "[--size <w>x<h>]\n"
        "                       --replay <video, image, folder of images or recording>...\n"
        "reading the MRZ, with --replay or --midv (the MRZ models beside --model):\n"
        "       --read every|paced [--reader-threads <n>] [--truth <file>] [--midv-templates <folder>]\n"
        "       [--band-stats] [--negatives <folder of pages with no MRZ>]\n");
    return 2;
}

std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::optional<std::string> {
            return i + 1 < argc ? std::optional<std::string>(argv[++i]) : std::nullopt;
        };
        if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        }
        if (arg.rfind("--", 0) != 0) {
            options.inputs.push_back(arg);
            continue;
        }
        if (arg == "--replay") {
            options.replay = true;
            continue;
        }
        if (arg == "--band-stats") {
            options.bandStats = true;
            continue;
        }
        const std::optional<std::string> v = value();
        if (!v) {
            return std::nullopt;
        }
        if (arg == "--model") {
            options.model = *v;
        } else if (arg == "--threads") {
            options.threads = std::stoi(*v);
        } else if (arg == "--margin") {
            options.margin = std::stod(*v);
        } else if (arg == "--sharpness") {
            options.sharpness = std::stod(*v);
        } else if (arg == "--size") {
            const std::size_t x = v->find('x');
            if (x == std::string::npos) {
                return std::nullopt;
            }
            options.size = cv::Size(std::stoi(v->substr(0, x)), std::stoi(v->substr(x + 1)));
        } else if (arg == "--labels") {
            options.labels = *v;
        } else if (arg == "--midv") {
            options.midv = *v;
        } else if (arg == "--frames") {
            options.frames = std::stoi(*v);
        } else if (arg == "--read") {
            if (*v != "every" && *v != "paced") {
                return std::nullopt;
            }
            options.read = *v;
        } else if (arg == "--reader-threads") {
            options.readerThreads = std::stoi(*v);
        } else if (arg == "--truth") {
            options.truth = *v;
        } else if (arg == "--midv-templates") {
            options.midvTemplates = *v;
        } else if (arg == "--negatives") {
            options.negatives = *v;
        } else {
            return std::nullopt;
        }
    }
    const int modes = (options.labels ? 1 : 0) + (options.midv ? 1 : 0) + (options.replay ? 1 : 0);
    if (options.model.empty() || modes != 1 || options.inputs.empty()) {
        return std::nullopt;
    }
    // The labelled loop reuses one cv::Mat for every frame, which a scan kept for Review would share.
    const bool reading =
        options.read || options.truth || options.midvTemplates || options.bandStats || options.negatives;
    if (options.labels && reading) {
        std::fprintf(stderr, "reading the MRZ works with --replay or --midv only\n");
        return std::nullopt;
    }
    if (!options.read && reading) {
        std::fprintf(stderr, "--truth, --midv-templates, --band-stats and --negatives need --read\n");
        return std::nullopt;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<Options> options;
    try {
        options = parse(argc, argv);
    } catch (const std::exception&) {
        options.reset(); // a number that is not one
    }
    if (!options) {
        return usage();
    }
    std::string error;
    doc_vision::OpenFailure failure = doc_vision::OpenFailure::None;
    const Clock::time_point openStart = Clock::now();
    const std::unique_ptr<doc_vision::PageDetector> detector =
        doc_vision::PageDetector::open(options->model, error, &failure, options->threads);
    const double openMs = msSince(openStart);
    if (!detector) {
        std::fprintf(stderr, "the model could not be opened: %s\n", error.c_str());
        return 1;
    }
    std::printf(
        "LC100 · threads %d · margin %.3f · sharpness %.0f · size %s · session created in %.1f ms\n",
        options->threads, thresholdsOf(*options).edgeMargin.failsAt, thresholdsOf(*options).sharpness.failsAt,
        options->size
            ? (std::to_string(options->size->width) + "x" + std::to_string(options->size->height)).c_str()
            : "as recorded",
        openMs);
    Run run;
    Scorer scorer(*options, *detector, run);
    if (options->labels) {
        runLabelled(*options, scorer, run);
    } else if (options->midv) {
        runMidv(*options, scorer, run);
    } else {
        runReplay(*options, scorer, run);
    }
    report(run);
    if (options->read) {
        reportReading(*options, run, scorer.log());
    }
    return 0;
}
