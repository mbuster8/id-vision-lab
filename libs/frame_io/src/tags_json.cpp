#include "tags_json.h"

#include <fstream>
#include <system_error>

namespace frame_io {

nlohmann::json toJson(const visual_debug::ImageDescription& tags) {
    nlohmann::json json = nlohmann::json::object();
    if (!tags.valueRange.empty()) {
        json["valueRange"] = {tags.valueRange.min, tags.valueRange.max};
    }
    if (tags.transfer) {
        switch (*tags.transfer) {
        case visual_debug::Transfer::Sdr:
            json["transfer"] = "sdr";
            break;
        case visual_debug::Transfer::Hlg:
            json["transfer"] = "hlg";
            break;
        case visual_debug::Transfer::Pq:
            json["transfer"] = "pq";
            break;
        }
    }
    if (tags.primaries) {
        json["primaries"] = *tags.primaries == visual_debug::Primaries::Bt2020 ? "bt2020" : "bt709";
    }
    return json;
}

visual_debug::ImageDescription tagsFromJson(const nlohmann::json& json) {
    visual_debug::ImageDescription tags;
    if (!json.is_object()) {
        return tags;
    }
    if (const auto range = json.find("valueRange"); range != json.end() && range->is_array() &&
                                                    range->size() == 2 && (*range)[0].is_number() &&
                                                    (*range)[1].is_number()) {
        tags.valueRange = {(*range)[0].get<double>(), (*range)[1].get<double>()};
    }
    if (const auto transfer = json.find("transfer"); transfer != json.end() && transfer->is_string()) {
        const std::string name = transfer->get<std::string>();
        if (name == "sdr") {
            tags.transfer = visual_debug::Transfer::Sdr;
        } else if (name == "hlg") {
            tags.transfer = visual_debug::Transfer::Hlg;
        } else if (name == "pq") {
            tags.transfer = visual_debug::Transfer::Pq;
        }
    }
    if (const auto primaries = json.find("primaries"); primaries != json.end() && primaries->is_string()) {
        const std::string name = primaries->get<std::string>();
        if (name == "bt709") {
            tags.primaries = visual_debug::Primaries::Bt709;
        } else if (name == "bt2020") {
            tags.primaries = visual_debug::Primaries::Bt2020;
        }
    }
    return tags;
}

namespace {

bool writeBytes(const std::filesystem::path& file, const char* data, std::size_t size, std::string& error) {
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!stream || !stream.write(data, static_cast<std::streamsize>(size))) {
            error = utf8Name(file) + ": could not be written";
            return false;
        }
    }
    std::error_code ignored; // Windows keeps the folder's access list, which these bits do not reach
    std::filesystem::permissions(file,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ignored);
    return true;
}

} // namespace

bool writeOwnerOnly(const std::filesystem::path& file, const std::vector<unsigned char>& bytes,
                    std::string& error) {
    return writeBytes(file, reinterpret_cast<const char*>(bytes.data()), bytes.size(), error);
}

bool writeOwnerOnly(const std::filesystem::path& file, const std::string& text, std::string& error) {
    return writeBytes(file, text.data(), text.size(), error);
}

std::string utf8Name(const std::filesystem::path& path) {
    const std::u8string text = path.filename().u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace frame_io
