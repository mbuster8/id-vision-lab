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

#include <doc_vision/guidance.h>
#include <doc_vision/onnx_model.h>
#include <doc_vision/page_detector.h>
#include <doc_vision/page_geometry.h>
#include <doc_vision/quality_gates.h>
#include <frame_io/recording.h>
#include <lab_core/document_processor.h>
#include <lab_core/focus_controller.h>

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
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
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
};

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

    void startClip() {
        m_processor = std::make_unique<lab_core::DocumentProcessor>(
            [this](std::string&, doc_vision::OpenFailure&) -> std::unique_ptr<doc_vision::PageFinder> {
                return std::make_unique<Forwarding>(m_finder);
            },
            m_thresholds);
        m_shown.reset();
        m_run.firstGood.emplace_back();
        // No control: the rule for a camera the lab cannot focus. Every frame of a replay counts at once.
        m_focus = std::make_unique<lab_core::FocusController>(nullptr, [this] { return m_number + 1; });
        m_focusState = lab_core::FocusStatus::State::Camera;
    }

    void frame(cv::Mat image, std::uint64_t number, double seconds, Group* group,
               const std::optional<Quad>& truth, const visual_debug::ImageDescription& tags = {}) {
        if (m_options.size && image.size() != *m_options.size) {
            cv::resize(image, image, *m_options.size, 0, 0, cv::INTER_LINEAR);
        }
        const frame_io::Frame frame{.image = image,
                                    .number = number,
                                    .at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::duration<double>(seconds)),
                                    .tags = tags};
        const Clock::time_point start = Clock::now();
        const lab_core::ProcessorResult result = m_processor->process(frame);
        m_run.timings.total.push_back(msSince(start));
        const Clock::time_point convertStart = Clock::now();
        const cv::Mat bgr = doc_vision::toBgr8(image, tags.valueRange.min, tags.valueRange.max);
        m_run.timings.convert.push_back(msSince(convertStart));

        if (!result.guidance) {
            return;
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
        if (shown == Hint::Good && !m_run.firstGood.back()) {
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
            return;
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
        group->frameGood += result.guidance->frameHint == Hint::Good ? 1 : 0;
        group->shownGood += shown == Hint::Good ? 1 : 0;
    }

private:
    const Options& m_options;
    doc_vision::Thresholds m_thresholds;
    TimedFinder m_finder;
    Run& m_run;
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
        run.seconds += i / fps;
    }
}

// Any replay the Document Lab opens, without labels: every frame at its own time, its tags too (a 16-bit
// recording's value range), into one group.
void runReplay(const Options& options, Scorer& scorer, Run& run) {
    for (std::size_t c = 0; c < options.inputs.size(); ++c) {
        std::string error;
        const std::unique_ptr<frame_io::ReplaySource> source =
            frame_io::openReplay(std::filesystem::path(options.inputs[c]), error);
        if (!source) {
            std::fprintf(stderr, "source %zu: cannot be opened: %s\n", c + 1, error.c_str());
            continue;
        }
        scorer.startClip();
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
            scorer.frame(read.frame.image, read.frame.number, last, &run.groups["all"], std::nullopt,
                         read.frame.tags);
        }
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
        for (int c = 0; c < 100; ++c) {
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
            scorer.startClip();
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
                scorer.frame(image, static_cast<std::uint64_t>(frame.number),
                             (frame.number - firstNumber) / 30.0, &run.groups[cut ? "cut" : "in view"],
                             cut ? std::nullopt : std::optional<Quad>(truth));
            }
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

int usage() {
    std::fprintf(stderr,
                 "usage: doc_vision_eval --model <onnx> [--threads <n>] [--margin <share>] [--sharpness <v>] "
                 "[--size <w>x<h>]\n"
                 "                       --labels <labels.csv> <clip>...\n"
                 "       doc_vision_eval --model <onnx> [--threads <n>] [--margin <share>] [--sharpness <v>] "
                 "[--size <w>x<h>]\n"
                 "                       --midv <clips folder> [--frames <n>] <type>...\n"
                 "       doc_vision_eval --model <onnx> [--threads <n>] [--margin <share>] [--sharpness <v>] "
                 "[--size <w>x<h>]\n"
                 "                       --replay <video, image, folder of images or recording>...\n");
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
        } else {
            return std::nullopt;
        }
    }
    const int modes = (options.labels ? 1 : 0) + (options.midv ? 1 : 0) + (options.replay ? 1 : 0);
    if (options.model.empty() || modes != 1 || options.inputs.empty()) {
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
    return 0;
}
