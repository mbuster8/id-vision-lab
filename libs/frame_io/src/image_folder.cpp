#include "frame_io/image_folder.h"

#include "file_sequence.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace frame_io {
namespace {

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// The end of the run of digits that starts at `from`.
std::size_t digitsEnd(std::string_view text, std::size_t from) {
    while (from < text.size() && isDigit(text[from])) {
        ++from;
    }
    return from;
}

// Leading zeros skipped, keeping at least one digit.
std::size_t significantStart(std::string_view text, std::size_t from, std::size_t end) {
    while (from + 1 < end && text[from] == '0') {
        ++from;
    }
    return from;
}

const char* depthName(int depth) {
    switch (depth) {
    case CV_8U:
        return "8U";
    case CV_8S:
        return "8S";
    case CV_16U:
        return "16U";
    case CV_16S:
        return "16S";
    case CV_32S:
        return "32S";
    case CV_32F:
        return "32F";
    case CV_64F:
        return "64F";
    case CV_16F:
        return "16F";
    default:
        return "an unusual depth";
    }
}

// A frame of the folder: 30 a second, in whole nanoseconds, truncated.
std::chrono::nanoseconds folderTime(std::uint64_t index) {
    return std::chrono::nanoseconds(static_cast<std::int64_t>(index) * 1'000'000'000 / 30);
}

// The name for messages and the window: as UTF-8, whatever the platform's own encoding.
std::string displayName(const std::filesystem::path& path) {
    // A folder given with a separator at its end has an empty file name; its own is the parent's.
    const std::filesystem::path named = path.has_filename() ? path : path.parent_path();
    const std::u8string name = named.filename().u8string();
    return std::string(name.begin(), name.end());
}

class ImageFile final : public ReplaySource {
public:
    ImageFile(Frame frame, std::string name) : m_frame(std::move(frame)), m_name(std::move(name)) {}

    ReadResult next() override {
        if (m_taken) {
            return {};
        }
        m_taken = true;
        return {.status = ReadStatus::Frame, .frame = m_frame, .error = {}};
    }
    bool ready() const override { return true; }
    void setWake(std::function<void()> /*wake*/) override {} // always ready
    bool restart(std::string& /*error*/) override {
        m_taken = false;
        return true;
    }
    bool canSeek() const override { return true; }
    void seek(std::uint64_t index) override { m_taken = index > 0; }
    std::optional<std::uint64_t> frameCount() const override { return 1; }
    std::string describe() const override { return m_name; }

private:
    Frame m_frame;
    std::string m_name;
    bool m_taken = false;
};

} // namespace

bool naturalLess(std::string_view a, std::string_view b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            const std::size_t aEnd = digitsEnd(a, i);
            const std::size_t bEnd = digitsEnd(b, j);
            const std::size_t aStart = significantStart(a, i, aEnd);
            const std::size_t bStart = significantStart(b, j, bEnd);
            const std::string_view aNumber = a.substr(aStart, aEnd - aStart);
            const std::string_view bNumber = b.substr(bStart, bEnd - bStart);
            if (aNumber.size() != bNumber.size()) {
                return aNumber.size() < bNumber.size();
            }
            if (const int compared = aNumber.compare(bNumber); compared != 0) {
                return compared < 0;
            }
            if (aEnd - i != bEnd - j) {
                return aEnd - i < bEnd - j; // fewer leading zeros first
            }
            i = aEnd;
            j = bEnd;
            continue;
        }
        if (a[i] != b[j]) {
            return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
        }
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

bool isImageFileName(const std::filesystem::path& file) {
    static constexpr std::array<std::string_view, 20> kExtensions{
        ".png", ".jpg", ".jpeg", ".jpe", ".tif", ".tiff", ".bmp", ".dib", ".webp", ".pbm",
        ".pgm", ".ppm", ".pnm",  ".sr",  ".ras", ".jp2",  ".exr", ".hdr", ".pic",  ".pxm"};
    const std::u8string raw = file.extension().u8string();
    std::string extension(raw.begin(), raw.end());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

std::vector<std::filesystem::path> imageFilesIn(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        std::error_code isFileError;
        if (entry.is_regular_file(isFileError) && isImageFileName(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return naturalLess(displayName(left), displayName(right));
    });
    return files;
}

cv::Mat readImage(const std::filesystem::path& file, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = displayName(file) + ": cannot be opened";
        return {};
    }
    const std::vector<uchar> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    cv::Mat image;
    try {
        image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    } catch (const cv::Exception&) {
        image.release(); // reported below like any image that does not decode
    }
    if (image.empty()) {
        error = displayName(file) + ": not an image OpenCV can read";
        return {};
    }
    if (!isAcceptedImage(image)) {
        error = displayName(file) + ": " + depthName(image.depth()) + " with " +
                std::to_string(image.channels()) +
                " channels; only 8- or 16-bit images with 1, 3 or 4 channels";
        return {};
    }
    return image;
}

std::unique_ptr<ReplaySource> openImageFolder(const std::filesystem::path& folder, std::string& error,
                                              ReadOptions options) {
    const std::vector<std::filesystem::path> files = imageFilesIn(folder);
    if (files.empty()) {
        error = displayName(folder) + " holds no images";
        return nullptr;
    }
    std::vector<SequenceEntry> entries;
    for (std::uint64_t index = 0; index < files.size(); ++index) {
        entries.push_back({.file = files[index], .at = folderTime(index), .tags = std::nullopt});
    }
    return std::make_unique<FileSequence>(std::move(entries), displayName(folder), std::move(options));
}

std::unique_ptr<ReplaySource> openImageFile(const std::filesystem::path& file, std::string& error) {
    cv::Mat image = readImage(file, error);
    if (image.empty()) {
        return nullptr;
    }
    Frame frame{.image = image, .number = 0, .at = {}, .tags = tagsFor(image, {})};
    return std::make_unique<ImageFile>(std::move(frame), displayName(file));
}

FileSequence::FileSequence(std::vector<SequenceEntry> entries, std::string description, ReadOptions options)
    : m_entries(std::move(entries)), m_description(std::move(description)), m_options(std::move(options)) {
    if (m_options.depth == 0) {
        m_options.depth = 1;
    }
    startReads();
}

FileSequence::~FileSequence() {
    for (auto& [index, read] : m_reads) {
        read.wait();
    }
}

void FileSequence::startReads() {
    std::uint64_t index = m_reads.empty() ? m_next : m_reads.back().first + 1;
    while (m_reads.size() < m_options.depth && index < m_entries.size()) {
        const SequenceEntry entry = m_entries[index];
        m_reads.emplace_back(
            index,
            std::async(std::launch::async, [entry, index, before = m_options.beforeRead, wake = m_wake] {
                if (before) {
                    before(index);
                }
                ReadResult result;
                std::string error;
                cv::Mat image = readImage(entry.file, error);
                if (image.empty()) {
                    result = {.status = ReadStatus::Error, .frame = {}, .error = error};
                } else {
                    const visual_debug::ImageDescription tags = entry.tags.value_or(tagsFor(image, {}));
                    result = {.status = ReadStatus::Frame,
                              .frame = {.image = image, .number = index, .at = entry.at, .tags = tags},
                              .error = {}};
                }
                std::function<void()> call;
                {
                    const std::lock_guard lock(wake->mutex);
                    call = wake->call;
                }
                if (call) {
                    call();
                }
                return result;
            }));
        ++index;
    }
}

ReadResult FileSequence::next() {
    if (m_next >= m_entries.size()) {
        return {};
    }
    startReads();
    ReadResult result;
    try {
        result = m_reads.front().second.get();
    } catch (...) {
        // A read that threw is an error at its frame, never the end of the folder.
        result = {.status = ReadStatus::Error,
                  .frame = {},
                  .error = displayName(m_entries[m_next].file) + ": reading failed"};
    }
    m_reads.pop_front();
    ++m_next;
    startReads();
    return result;
}

bool FileSequence::ready() const {
    if (m_next >= m_entries.size() || m_reads.empty()) {
        return true;
    }
    return m_reads.front().second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void FileSequence::setWake(std::function<void()> wake) {
    const std::lock_guard lock(m_wake->mutex);
    m_wake->call = std::move(wake);
}

bool FileSequence::restart(std::string& /*error*/) {
    seek(0);
    return true;
}

bool FileSequence::canSeek() const { return true; }

void FileSequence::seek(std::uint64_t index) {
    for (auto& [readIndex, read] : m_reads) {
        read.wait();
    }
    m_reads.clear();
    m_next = std::min<std::uint64_t>(index, m_entries.size());
    startReads();
}

std::optional<std::uint64_t> FileSequence::frameCount() const { return m_entries.size(); }

std::string FileSequence::describe() const { return m_description; }

} // namespace frame_io
