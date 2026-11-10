#pragma once

// A replay over a list of reads, always ready, for the engine's and the Document Lab's tests. A video file
// cannot seek; its restart() opens it again, which the counters count.

#include <frame_io/replay_source.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

// A 4×3 picture of the frame's number, or `image` when given.
inline frame_io::Frame listFrame(std::uint64_t number, std::chrono::nanoseconds at, cv::Mat image = {}) {
    if (image.empty()) {
        image = cv::Mat(3, 4, CV_8UC3, cv::Scalar::all(static_cast<double>(number % 256)));
    }
    return {.image = std::move(image), .number = number, .at = at, .tags = {}};
}

inline std::vector<frame_io::ReadResult> framesApart(int count, std::chrono::nanoseconds gap) {
    std::vector<frame_io::ReadResult> reads;
    for (int i = 0; i < count; ++i) {
        const auto number = static_cast<std::uint64_t>(i);
        reads.push_back(
            {.status = frame_io::ReadStatus::Frame, .frame = listFrame(number, i * gap), .error = {}});
    }
    return reads;
}

class ListSource : public frame_io::ReplaySource {
public:
    struct Counters {
        int restarts = 0;
        int seeks = 0;
        std::optional<std::uint64_t> lastSeek{};
    };

    ListSource(std::vector<frame_io::ReadResult> reads, bool seekable, Counters* counters)
        : m_reads(std::move(reads)), m_seekable(seekable), m_counters(counters) {}

    // Whether a frame is there yet, as a video's reading thread would say during a seek; always by default.
    std::function<bool()> readyWhen{};
    // next() throws std::runtime_error("disk gone") when it would read the frame at this index.
    std::optional<std::uint64_t> throwAtNext{};

    frame_io::ReadResult next() override {
        if (throwAtNext && m_index == *throwAtNext) {
            throw std::runtime_error("disk gone");
        }
        if (m_index >= m_reads.size()) {
            return {.status = frame_io::ReadStatus::End, .frame = {}, .error = {}};
        }
        return m_reads[m_index++];
    }
    bool ready() const override { return !readyWhen || readyWhen(); }
    void setWake(std::function<void()>) override {}
    bool restart(std::string&) override {
        m_index = 0;
        if (m_counters) {
            ++m_counters->restarts;
        }
        return true;
    }
    bool canSeek() const override { return m_seekable; }
    void seek(std::uint64_t index) override {
        m_index = static_cast<std::size_t>(index);
        if (m_counters) {
            ++m_counters->seeks;
            m_counters->lastSeek = index;
        }
    }
    std::optional<std::uint64_t> frameCount() const override {
        std::uint64_t frames = 0;
        for (const frame_io::ReadResult& read : m_reads) {
            frames += read.status == frame_io::ReadStatus::Frame ? 1 : 0;
        }
        return frames;
    }
    std::optional<double> frameRate() const override { return 30.0; }
    std::string describe() const override { return "list"; }

private:
    std::vector<frame_io::ReadResult> m_reads;
    std::size_t m_index = 0;
    bool m_seekable;
    Counters* m_counters;
};

// An opener for the engine: a new ListSource over the reads each time.
inline std::function<std::unique_ptr<frame_io::ReplaySource>(std::string&)>
openerOf(std::vector<frame_io::ReadResult> reads, bool seekable = true,
         ListSource::Counters* counters = nullptr) {
    return [reads = std::move(reads), seekable,
            counters](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        return std::make_unique<ListSource>(reads, seekable, counters);
    };
}

} // namespace test_support
