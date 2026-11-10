#pragma once

// A replay source over a list of image files, read ahead in order: what a folder of images and a recording
// both are. Private to frame_io.

#include "frame_io/image_folder.h"
#include "frame_io/replay_source.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frame_io {

struct SequenceEntry {
    std::filesystem::path file;
    std::chrono::nanoseconds at{};
    std::optional<visual_debug::ImageDescription> tags{}; // nothing: tagsFor(image, {})
};

class FileSequence final : public ReplaySource {
public:
    FileSequence(std::vector<SequenceEntry> entries, std::string description, ReadOptions options,
                 double rate);
    ~FileSequence() override; // waits for the reads in flight

    FileSequence(const FileSequence&) = delete;
    FileSequence& operator=(const FileSequence&) = delete;

    ReadResult next() override;
    bool ready() const override;
    void setWake(std::function<void()> wake) override;
    bool restart(std::string& error) override;
    bool canSeek() const override;
    void seek(std::uint64_t index) override;
    std::optional<std::uint64_t> frameCount() const override;
    std::optional<double> frameRate() const override;
    std::string describe() const override;

private:
    // The wake, shared with the reads in flight, which may outlive a setWake.
    struct Wake {
        std::mutex mutex;
        std::function<void()> call;
    };

    void startReads(); // starts the reads of the frames from m_next up to `depth` ahead not started yet

    std::vector<SequenceEntry> m_entries;
    std::string m_description;
    ReadOptions m_options;
    double m_rate;
    std::shared_ptr<Wake> m_wake = std::make_shared<Wake>();
    std::uint64_t m_next = 0;                                              // the index next() returns
    std::deque<std::pair<std::uint64_t, std::future<ReadResult>>> m_reads; // from m_next on, in order
};

} // namespace frame_io
