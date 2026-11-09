#include "frame_io/recording.h"

#include "tags_json.h"

#include <opencv2/imgcodecs.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>

namespace frame_io {
namespace {

constexpr char kJsonName[] = "recording.json";
constexpr char kJsonTemporaryName[] = "recording.json.tmp";

// 000000.png …: six digits, more once a recording passes a million frames.
std::string fileNameFor(std::uint64_t index) {
    std::string digits = std::to_string(index);
    if (digits.size() < 6) {
        digits.insert(0, 6 - digits.size(), '0');
    }
    return digits + ".png";
}

std::uintmax_t availableSpace(const std::filesystem::path& folder) {
    std::error_code error;
    const std::filesystem::space_info space = std::filesystem::space(folder, error);
    // A volume that cannot say is not taken for a full one.
    return error ? std::numeric_limits<std::uintmax_t>::max() : space.available;
}

} // namespace

std::filesystem::path createUniqueFolder(const std::filesystem::path& parent, const std::string& baseName,
                                         std::string& error) {
    for (int attempt = 1; attempt < 1000; ++attempt) {
        const std::string name = attempt == 1 ? baseName : baseName + " " + std::to_string(attempt);
        const std::filesystem::path folder = parent / fromUtf8(name);
        std::error_code failed;
        if (std::filesystem::create_directory(folder, failed)) {
            return folder;
        }
        if (failed) {
            error = utf8Name(folder) + ": " + failed.message();
            return {};
        }
    }
    error = "no free name for " + baseName;
    return {};
}

std::unique_ptr<RecordingWriter> RecordingWriter::start(std::filesystem::path folder, RecordingInfo info,
                                                        Options options, std::string& error) {
    std::unique_ptr<RecordingWriter> writer(
        new RecordingWriter(std::move(folder), std::move(info), std::move(options)));
    {
        const std::lock_guard lock(writer->m_mutex);
        if (!writer->writeJson(false, error)) {
            return nullptr;
        }
    }
    if (writer->m_options.mode == Mode::Threaded) {
        for (int i = 0; i < std::max(1, writer->m_options.threads); ++i) {
            writer->m_threads.emplace_back([raw = writer.get()] { raw->writeLoop(); });
        }
    }
    return writer;
}

RecordingWriter::RecordingWriter(std::filesystem::path folder, RecordingInfo info, Options options)
    : m_folder(std::move(folder)), m_info(std::move(info)), m_options(std::move(options)) {
    if (!m_options.freeSpace) {
        m_options.freeSpace = availableSpace;
    }
}

RecordingWriter::~RecordingWriter() { finish(); }

RecordingWriter::Push RecordingWriter::push(const Frame& frame) {
    if (m_options.freeSpace(m_folder) < m_options.minimumFreeBytes) {
        return Push::LowDisk;
    }
    const std::size_t bytes = frame.image.total() * frame.image.elemSize();
    {
        const std::lock_guard lock(m_mutex);
        // A frame larger than the whole queue still goes in, alone.
        if (m_finishing || (m_bytes > 0 && m_bytes + bytes > m_options.queueBytes)) {
            return Push::Full;
        }
        const std::uint64_t index = m_entries.size();
        if (!m_firstAt) {
            m_firstAt = frame.at;
        }
        m_entries.push_back({.file = fileNameFor(index), .at = frame.at - *m_firstAt, .tags = frame.tags});
        m_jobs.push_back(
            {.index = index, .image = frame.image, .file = m_folder / fileNameFor(index), .bytes = bytes});
        m_bytes += bytes;
    }
    m_changed.notify_all();
    return Push::Queued;
}

bool RecordingWriter::waitForSpace(std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, timeout, [this] { return m_bytes < m_options.queueBytes; });
}

bool RecordingWriter::pump() {
    bool wrote = false;
    for (;;) {
        Job job;
        {
            const std::lock_guard lock(m_mutex);
            if (m_jobs.empty()) {
                break;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            ++m_writing;
        }
        write(std::move(job));
        wrote = true;
    }
    return wrote;
}

void RecordingWriter::countNotRecorded() {
    const std::lock_guard lock(m_mutex);
    ++m_notRecorded;
}

void RecordingWriter::setCounts(const RecordingCounts& counts) {
    const std::lock_guard lock(m_mutex);
    m_counts = counts;
}

RecordingWriter::Progress RecordingWriter::progress() const {
    const std::lock_guard lock(m_mutex);
    return {.written = m_written,
            .queued = m_jobs.size() + static_cast<std::uint64_t>(m_writing),
            .notRecorded = m_notRecorded,
            .finished = m_finished,
            .failure = m_failure};
}

void RecordingWriter::finishAsync() {
    if (m_options.mode == Mode::Immediate) {
        pump();
    }
    {
        const std::lock_guard lock(m_mutex);
        m_finishing = true;
        completeIfDone();
    }
    m_changed.notify_all();
}

void RecordingWriter::finish() {
    finishAsync();
    {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return m_finished; });
    }
    for (std::thread& thread : m_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void RecordingWriter::writeLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_changed.wait(lock, [this] { return !m_jobs.empty() || m_finishing; });
            if (m_jobs.empty()) {
                return; // finishing, and nothing left to write
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            ++m_writing;
        }
        write(std::move(job));
    }
}

void RecordingWriter::write(Job job) {
    if (m_options.beforeWrite) {
        m_options.beforeWrite(job.index);
    }
    std::string failure;
    std::vector<unsigned char> bytes;
    try {
        if (!cv::imencode(".png", job.image, bytes)) {
            failure = utf8Name(job.file) + ": the frame could not be encoded as PNG";
        }
    } catch (const cv::Exception&) {
        failure = utf8Name(job.file) + ": the frame could not be encoded as PNG";
    }
    if (failure.empty()) {
        writeOwnerOnly(job.file, bytes, failure);
    }
    job.image.release(); // before the queue has room again
    {
        const std::lock_guard lock(m_mutex);
        wrote(job.bytes, failure);
    }
    m_changed.notify_all();
}

void RecordingWriter::wrote(std::size_t bytes, const std::string& failure) {
    --m_writing;
    m_bytes -= bytes;
    ++m_written;
    if (!failure.empty() && m_failure.empty()) {
        m_failure = failure;
    }
    completeIfDone();
}

void RecordingWriter::completeIfDone() {
    if (!m_finishing || m_finished || !m_jobs.empty() || m_writing > 0) {
        return;
    }
    std::string error;
    if (!writeJson(true, error) && m_failure.empty()) {
        m_failure = error;
    }
    m_finished = true;
}

bool RecordingWriter::writeJson(bool complete, std::string& error) const {
    nlohmann::json frames = nlohmann::json::array();
    const visual_debug::ImageDescription first =
        m_entries.empty() ? visual_debug::ImageDescription{} : m_entries.front().tags;
    for (const Entry& entry : m_entries) {
        nlohmann::json frame{{"file", entry.file}, {"at", entry.at.count()}};
        if (entry.tags != first) {
            frame["tags"] = toJson(entry.tags);
        }
        frames.push_back(std::move(frame));
    }
    const nlohmann::json json{
        {"format", "document-lab-recording"},
        {"version", 1},
        {"complete", complete},
        {"startedAt", m_info.startedAt},
        {"source", {{"kind", m_info.kind}, {"name", m_info.name}, {"format", m_info.format}}},
        {"tags", toJson(first)},
        {"frames", std::move(frames)},
        {"counts",
         {{"droppedBeforeProcessing", m_counts.droppedBeforeProcessing},
          {"notDrawn", m_counts.notDrawn},
          {"notSent", m_counts.notSent},
          {"notRecorded", m_counts.notRecorded}}}};
    // Through a temporary file and a rename, so a crash leaves the old file or the new one, never half of
    // one.
    const std::filesystem::path temporary = m_folder / kJsonTemporaryName;
    if (!writeOwnerOnly(temporary, json.dump(2) + "\n", error)) {
        return false;
    }
    std::error_code failed;
    std::filesystem::rename(temporary, m_folder / kJsonName, failed);
    if (failed) {
        error = std::string(kJsonName) + ": " + failed.message();
        return false;
    }
    return true;
}

} // namespace frame_io
