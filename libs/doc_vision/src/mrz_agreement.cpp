#include "doc_vision/mrz_agreement.h"

#include <algorithm>

namespace doc_vision {

std::optional<std::string> MrzAgreement::add(std::string text, std::uint64_t frame) {
    if (holds(frame)) {
        return std::nullopt;
    }
    m_reads.emplace_back(std::move(text), frame);
    while (m_reads.size() > kAgreementWindow) {
        m_reads.pop_front();
    }
    const std::string& added = m_reads.back().first;
    if (occurrences(added) >= kAgreeingReads) {
        return added;
    }
    return std::nullopt;
}

int MrzAgreement::count() const {
    int most = 0;
    for (const auto& read : m_reads) {
        most = std::max(most, occurrences(read.first));
    }
    return std::min(most, kAgreeingReads);
}

bool MrzAgreement::holds(std::uint64_t frame) const {
    return std::any_of(m_reads.begin(), m_reads.end(),
                       [frame](const auto& read) { return read.second == frame; });
}

int MrzAgreement::occurrences(const std::string& text) const {
    return static_cast<int>(std::count_if(m_reads.begin(), m_reads.end(),
                                          [&text](const auto& read) { return read.first == text; }));
}

} // namespace doc_vision
