#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace test_support {

// A folder of its own under the system's temporary one, removed afterwards. Random, as test programs run in
// parallel.
class ScratchFolder {
public:
    ScratchFolder()
        : m_path(std::filesystem::temp_directory_path() /
                 ("frame_io_test_" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(m_path);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ~ScratchFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    std::string file(const std::string& name) const { return (m_path / name).string(); }
    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace test_support
