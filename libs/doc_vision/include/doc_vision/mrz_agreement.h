#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace doc_vision {

// M of the last K reads (ADR-004 decision 9): an MRZ is accepted when its whole text occurs this often...
inline constexpr int kAgreeingReads = 3;
// ...among this many of the newest valid reads.
inline constexpr std::size_t kAgreementWindow = 10;

// Agreement across frames on the whole MRZ: valid reads compared as whole strings, each of a different frame.
// Nothing per character: no voting, weighting, alignment or estimate (US 11,640,720 and US 11,816,910).
class MrzAgreement {
public:
    // A valid read's whole text, of frame `frame`; a second read of a frame already in the window is ignored
    // (three reads of the same pixels are no evidence). The text, once it occurs kAgreeingReads times in the
    // window, on the add that makes it so.
    std::optional<std::string> add(std::string text, std::uint64_t frame);
    // The most times any one text occurs in the window, at most kAgreeingReads.
    int count() const;
    std::size_t size() const { return m_reads.size(); }
    bool holds(std::uint64_t frame) const;
    void clear() { m_reads.clear(); }

private:
    int occurrences(const std::string& text) const;

    std::deque<std::pair<std::string, std::uint64_t>> m_reads; // oldest first
};

} // namespace doc_vision
