#pragma once

#include "doc_vision/page_geometry.h"
#include "doc_vision/quality_gates.h"

#include <array>
#include <chrono>
#include <optional>
#include <utility>

namespace doc_vision {

// The one thing the banner tells the user, the first that applies in this order (ADR-004's table).
enum class Hint {
    NoPage,
    CutByEdge,
    MoveToMiddle, // cut by the frame's edge, but small enough to fit
    TooFar,
    TooClose,
    Tilted,
    Blurred,
    Glare,
    TooDark,
    TooBright,
    Good,
    // A4: every gate passes, and the MRZ reader says more (the Document processor sets these, never hintFor).
    NoMrzBand,     // hint 11: the last read found no MRZ band
    CannotReadMrz, // hint 12: 5 s without an accept
};

// Neutral: no page yet. Bad: something to change. Good: hold still.
enum class Tone { Neutral, Bad, Good };

Tone toneOf(Hint hint);
// The gate a Bad hint is about, whose chip shows ✗ while the hint shows.
std::optional<Gate> gateOf(Hint hint);
// Every gate passes with this hint: Good, and the MRZ's hints 11 and 12.
bool gatesPass(Hint hint);
// The frame's hint: NoPage without verdicts, else the first gate that fails, else Good.
Hint hintFor(const std::optional<Verdicts>& verdicts);

// Good shows once it has held this long.
inline constexpr std::chrono::nanoseconds kGoodAfter = std::chrono::milliseconds{500};
// A hint replaces a shown Good once it has held this long.
inline constexpr std::chrono::nanoseconds kLeaveGoodAfter = std::chrono::milliseconds{100};
// Any other hint shows once it has held this long...
inline constexpr std::chrono::nanoseconds kShowAfter = std::chrono::milliseconds{300};
// ...and a shown hint other than Good stays at least this long.
inline constexpr std::chrono::nanoseconds kStayAtLeast = std::chrono::milliseconds{800};
// A lost page is kept this long: a third of a second.
inline constexpr std::chrono::nanoseconds kHoldFor{333'333'333};

// The hint the banner shows, steadied by the frames' own times: the first hint of a run at once; Good once
// the frame's hint has been Good without a break for kGoodAfter; away from a shown Good after
// kLeaveGoodAfter; any other change after kShowAfter, and only once the shown hint has stayed kStayAtLeast.
// Bad news fast, good news slow, and no flicker either way.
class HintTimer {
public:
    Hint next(Hint frameHint, std::chrono::nanoseconds at);
    void reset();

private:
    bool m_started = false;
    Hint m_shown = Hint::NoPage;
    std::chrono::nanoseconds m_shownSince{};
    Hint m_candidate = Hint::NoPage;
    std::chrono::nanoseconds m_candidateSince{};
};

// The six chips under the canvas.
struct ChipStates {
    bool visible = false;
    std::array<bool, kGateCount> passed{};
    bool operator==(const ChipStates&) const = default;
};

// The chips, kept in step with the banner: visible exactly while the shown hint is not NoPage; all ✓ while
// Good shows; ✗ for the gate of a Bad hint that shows; otherwise each follows its gate's verdict once that
// verdict has held kShowAfter (its first state at once).
class ChipTimer {
public:
    ChipStates next(const std::optional<Verdicts>& verdicts, Hint shown, std::chrono::nanoseconds at);
    void reset();

private:
    struct Chip {
        bool started = false;
        bool shown = true;
        bool candidate = true;
        std::chrono::nanoseconds candidateSince{};
    };
    std::array<Chip, kGateCount> m_chips{};
};

// A page and its verdicts, as the hold last saw them.
struct Held {
    Quad quad{};
    Verdicts verdicts{};
    bool fresh = true; // found in this frame, not held over from an earlier one
};

// A page lost for no longer than kHoldFor keeps its last quad and verdicts, so a missed detection does not
// change the hint, hide the chips or move the outline.
class PageHold {
public:
    std::optional<Held> next(const std::optional<std::pair<Quad, Verdicts>>& found,
                             std::chrono::nanoseconds at);
    void reset();

private:
    std::optional<Held> m_last;
    std::chrono::nanoseconds m_lastAt{};
};

} // namespace doc_vision
