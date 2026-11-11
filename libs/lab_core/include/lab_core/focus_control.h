#pragma once

// A camera's own focus, as the lab can set it, and what the lab's focus controller reports (A3.3). No project
// includes: the Windows control's file includes this beside <windows.h>, whose Polygon clashes with
// visual_debug's.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace lab_core {

// The lens positions a camera offers: from minimum to maximum in steps of step.
struct FocusRange {
    int minimum = 0;
    int maximum = 0;
    int step = 1;
    bool operator==(const FocusRange&) const = default;
};

// What the camera's focus was before the lab first changed it in a run: put back when the lab lets go.
struct FocusFound {
    bool autoFocus = true;
    std::optional<int> position{}; // what to put back when autofocus was off; nothing when unreadable
};

// What the focus controller is doing, with each processed camera frame.
struct FocusStatus {
    enum class State { Camera, Searching, Holding, CannotFocus };
    enum class Kind { Page, Click };
    State state = State::Camera;
    Kind kind = Kind::Page;         // of the search running or the focus held
    bool controllable = false;      // the lab can set this camera's focus, and has not given up on it
    bool afterSearch = false;       // CannotFocus because a search failed, not for want of a control
    std::optional<int> position{};  // the lens, while the lab searches or holds
    std::string note{};             // the latest: "Focused at 30 in 3.1 s", "Could not focus there", …
    std::uint64_t noteNumber = 0;   // grows with each new note
    bool restoreFailed = false;     // putting the camera's focus back failed in this run (sticky)
    std::uint64_t countingFrom = 0; // the current step's first frame number that may count, while searching
    std::optional<std::chrono::nanoseconds> blurredFirstSince{}; // when sharpness began to fail first
    bool operator==(const FocusStatus&) const = default;
};

// A camera's own focus control. Called on the processing thread only; it opens what it needs there, on the
// first call. Every method is noexcept: a failure is false or nothing. Constructing one, and destroying one
// that never opened, makes no native call.
class FocusControl {
public:
    FocusControl() = default;
    FocusControl(const FocusControl&) = delete;
    FocusControl& operator=(const FocusControl&) = delete;
    virtual ~FocusControl() = default;

    // Nothing: this camera's focus cannot be set from the lab (no absolute focus, or it cannot be reached).
    virtual std::optional<FocusRange> range() noexcept = 0;
    // The camera offers autofocus that the lab can turn on and off.
    virtual bool hasAutoFocus() noexcept = 0;
    virtual std::optional<bool> autoFocus() noexcept = 0;
    virtual bool setAutoFocus(bool on) noexcept = 0;
    // Where the lens is now, moved by the camera's autofocus or by the lab.
    virtual std::optional<int> position() noexcept = 0;
    // The caller turns autofocus off first.
    virtual bool setPosition(int position) noexcept = 0;
};

} // namespace lab_core
