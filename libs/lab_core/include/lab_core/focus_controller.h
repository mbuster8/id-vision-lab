#pragma once

#include "lab_core/focus_control.h"
#include "lab_core/frame_processor.h"

#include <doc_vision/page_geometry.h>
#include <doc_vision/quality_gates.h>
#include <frame_io/frame.h>

#include <opencv2/core/types.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lab_core {

// A3.3's focus rules (spec §4.1). Unmeasured ones are marked; the rest come from the focus spike (spec §12).
// Steady: a fresh page in this share of the frames of the last kSteadyWindow, every corner within
// kSteadyCorners of the frame's longer side of where it was at the window's start (unmeasured).
inline constexpr std::chrono::nanoseconds kSteadyWindow = std::chrono::milliseconds{500};
inline constexpr double kSteadyFound = 0.70;
inline constexpr double kSteadyCorners = 0.02;
// Sharpness the first gate failing for this long, on a steady page, before the lab searches.
inline constexpr std::chrono::nanoseconds kTakeOverAfter = std::chrono::milliseconds{1500};
// No page search this soon after any search ended.
inline constexpr std::chrono::nanoseconds kCoolDown = std::chrono::seconds{3};
// A camera the lab cannot focus is told so after this long.
inline constexpr std::chrono::nanoseconds kCannotFocusAfter = std::chrono::seconds{3};
// No fresh page for this long: the page is lost.
inline constexpr std::chrono::nanoseconds kLostAfter = std::chrono::seconds{1};
// The page's longer side changed by more than this share: another page, or the same moved nearer or further.
inline constexpr double kSizeChange = 0.15;
// Page searches for one page before it is lost or changes size; a local search and its full fallback are one.
inline constexpr int kSearchesPerMemory = 2;
// The coarse step: the range in this many parts, on the camera's grid (10 on the C920's 0 to 250).
inline constexpr int kCoarseDivisions = 25;
// After a move, a frame offered after it counts once this long has passed since the first such frame, plus
// kSettlePerUnit for each unit moved beyond kSettleFreeUnits (unmeasured in frame time: the spike timed 150
// ms from the move to the analysis's arrival for steps of 10).
inline constexpr std::chrono::nanoseconds kSettleBase = std::chrono::milliseconds{150};
inline constexpr std::chrono::nanoseconds kSettlePerUnit = std::chrono::milliseconds{2};
inline constexpr int kSettleFreeUnits = 10;
// A step's value: the mean of this many counting frames (single frames are noisy).
inline constexpr int kFramesPerStep = 2;
// The coarse scan stops after kEarlyStopSteps consecutive steps after the best, each under this share of it.
inline constexpr double kEarlyStopShare = 0.30;
inline constexpr int kEarlyStopSteps = 3;
// A search again while holding first tries this many coarse steps either side of the held position.
inline constexpr int kLocalSteps = 2;
// Any search ends after this long.
inline constexpr std::chrono::nanoseconds kSearchLimit = std::chrono::seconds{12};
// Click-to-focus: a square of this share of the frame's shorter side; success when the best is at least
// kClickFactor × the median of the steps and at least kClickFloor (the region's scale is not the page's).
inline constexpr double kClickRegionShare = 0.20;
inline constexpr double kClickFactor = 3.0;
inline constexpr double kClickFloor = 20.0;

// What the status bar says when the camera's own focus could not be put back.
inline constexpr const char* kFocusRestoreFailed =
    "The camera's autofocus could not be turned back on: unplug the camera and plug it in again.";

// One measured position of a search.
struct FocusStep {
    int position = 0;
    double sharpness = 0.0;
    std::uint64_t countingFrom = 0; // the first frame number that could count for it
};

// The results panel's Focus row (spec §3.4).
Finding focusFinding(const FocusStatus& status);

// The lab's focus for one camera run (spec §4.1): it watches the page the Document processor finds, searches
// the camera's focus when a steady page stays blurred, holds the best, searches a clicked region, and puts
// the camera's focus back as it found it. Timed by the frames' own `at`. Called on the processing thread
// only; nothing it calls throws out of it.
class FocusController {
public:
    // The number the engine will give the next camera frame it is offered; read after each move, so only
    // frames offered after the lens was told to move count.
    using NextOffer = std::function<std::uint64_t()>;

    // `control` null: the lab cannot set this camera's focus. `passesAt`: a page search succeeds from here.
    FocusController(std::unique_ptr<FocusControl> control, NextOffer nextOffer,
                    double passesAt = doc_vision::Thresholds{}.sharpness.passesAgainAt);
    ~FocusController();
    FocusController(const FocusController&) = delete;
    FocusController& operator=(const FocusController&) = delete;

    // One processed camera frame, with the page found in it if any.
    FocusStatus next(const frame_io::Frame& frame, const std::optional<PageFacts>& page);
    // A click at (x, y), shares of the frame; acted on at the next frame. Non-finite values are ignored.
    void requestAt(double x, double y);
    // Puts back what the lab changed in this run. False when that failed. A second call does nothing.
    bool end() noexcept;

    const FocusStatus& status() const { return m_status; }
    // The steps of the search running, or of the last one.
    const std::vector<FocusStep>& curve() const { return m_curve; }

private:
    struct Seen {
        std::chrono::nanoseconds at{};
        std::optional<doc_vision::Quad> quad{};
        double frameSide = 0.0; // the frame's longer side, in pixels
    };
    // The page the page searches were for: its side when the last one ended, how many ran, and whether the
    // last failed (no page search until the page is lost or changes size).
    struct Memory {
        double side = 0.0;
        int searches = 0;
        bool blocked = false;
    };
    enum class Stage { Coarse, Fine, Local };
    struct Search {
        FocusStatus::Kind kind = FocusStatus::Kind::Page;
        Stage stage = Stage::Coarse;
        std::vector<int> positions{};
        std::size_t index = 0;
        std::chrono::nanoseconds startedAt{};
        double side = 0.0;   // the page's longer side when it started
        cv::Rect region{};   // a click's
        double before = 0.0; // a click's region before any move
        int lensFrom = 0;    // where the lens was before the last move
        std::uint64_t waitFrom = 0;
        std::optional<std::chrono::nanoseconds> firstCountingAt{};
        std::chrono::nanoseconds settle{};
        std::vector<double> samples{};
        double best = -1.0;
        int bestPosition = 0;
        int below = 0; // consecutive steps after the best under kEarlyStopShare of it
    };

    void step(const frame_io::Frame& frame, const std::optional<PageFacts>& page);
    bool canFocus() const { return m_range.has_value() && m_reachable; }
    void checkControl();
    void watch(const frame_io::Frame& frame, const std::optional<PageFacts>& page);
    bool steady() const;
    bool lost(std::chrono::nanoseconds at) const;
    bool mayStartPageSearch(std::chrono::nanoseconds at) const;
    void startPageSearch(const frame_io::Frame& frame, const PageFacts& page, bool local);
    void startClickSearch(const frame_io::Frame& frame, double x, double y);
    bool takeControl();
    void searchFrame(const frame_io::Frame& frame, const std::optional<PageFacts>& page);
    void beginStage(Stage stage, std::vector<int> positions, const frame_io::Frame& frame);
    bool moveTo(int position, const frame_io::Frame& frame);
    void afterStep(const frame_io::Frame& frame);
    bool succeeded(const Search& search) const;
    void finishSearch(const frame_io::Frame& frame);
    void abortSearch(const frame_io::Frame& frame);
    void hold(const Search& search, const frame_io::Frame& frame);
    void letGo();
    void giveUp();
    bool restore();
    void setNote(std::string note);
    double clickValue(const frame_io::Frame& frame, cv::Rect region) const;
    std::optional<int> predicted(double side) const;
    std::vector<int> coarsePositions(int start) const;
    int top() const;
    int onGrid(int position) const;
    int coarseStep() const;
    int fineStep() const;

    std::unique_ptr<FocusControl> m_control;
    NextOffer m_nextOffer;
    double m_passesAt;
    bool m_checked = false;
    std::optional<FocusRange> m_range;
    bool m_reachable = true;
    bool m_hasAuto = false;
    bool m_autoOff = false; // the lab turned the camera's autofocus off
    std::optional<FocusFound> m_found;
    bool m_changed = false; // the lab changed the camera's focus since the last restore
    FocusStatus m_status;
    std::vector<FocusStep> m_curve;

    std::deque<Seen> m_window;
    std::optional<std::chrono::nanoseconds> m_lastFreshAt;
    double m_lastSide = 0.0;
    std::optional<std::chrono::nanoseconds> m_blurredSince;
    std::optional<std::chrono::nanoseconds> m_lastSearchEnd;
    std::optional<Memory> m_memory;
    std::vector<std::pair<double, int>> m_predictions; // (longer side, position) of sharp page searches
    std::optional<Search> m_search;
    std::optional<std::pair<double, double>> m_request;
    // Holding and CannotFocus: the page's side when they began, and when the hold began (its loss clock).
    double m_heldSide = 0.0;
    std::chrono::nanoseconds m_heldSince{};
    double m_cannotSide = 0.0;
};

} // namespace lab_core
