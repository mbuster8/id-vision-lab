// Makes a Windows .ico from one square PNG at build time, so the repository holds only the PNG, as it does
// for the macOS bundles' .icns. Each size is a PNG inside the .ico, which Windows reads since Vista.
//
//     make_ico <input.png> <output.ico>
//
// Built and run on Windows only. wmain, std::filesystem and imdecode/imencode, because cv::imread and
// cv::imwrite take narrow paths there and a build folder may have any name.

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

namespace {

// The sizes Explorer, the taskbar and Alt-Tab use at 100 % to 200 % display scaling.
constexpr int kSizes[] = {256, 48, 32, 16};

std::vector<unsigned char> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Every channel layout as BGRA, so each size keeps or gains an alpha channel.
cv::Mat toBgra(const cv::Mat& image) {
    cv::Mat bgra;
    switch (image.channels()) {
    case 1:
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
        break;
    case 3:
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
        break;
    default:
        bgra = image;
        break;
    }
    return bgra;
}

void putLittleEndian(std::vector<unsigned char>& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFF));
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: make_ico <input.png> <output.ico>\n";
        return 2;
    }
    const std::vector<unsigned char> source = readFile(argv[1]);
    cv::Mat image = cv::imdecode(source, cv::IMREAD_UNCHANGED);
    if (image.empty() || image.depth() != CV_8U) {
        std::cerr << "make_ico: cannot read an 8-bit image from the input\n";
        return 1;
    }
    // Icons are square; a wider or taller picture would be stretched into them.
    if (image.cols != image.rows) {
        std::cerr << "make_ico: the image is " << image.cols << 'x' << image.rows << ", not square\n";
        return 1;
    }
    image = toBgra(image);

    std::vector<std::vector<unsigned char>> pngs;
    for (const int size : kSizes) {
        cv::Mat resized;
        cv::resize(image, resized, cv::Size(size, size), 0, 0, cv::INTER_AREA);
        std::vector<unsigned char> png;
        if (!cv::imencode(".png", resized, png)) {
            std::cerr << "make_ico: cannot encode a " << size << " pixel image\n";
            return 1;
        }
        pngs.push_back(std::move(png));
    }

    // ICONDIR, then one 16-byte ICONDIRENTRY per image, then the images. A width or height of 0 means 256.
    std::vector<unsigned char> ico;
    putLittleEndian(ico, 0, 2); // reserved
    putLittleEndian(ico, 1, 2); // an icon, not a cursor
    putLittleEndian(ico, static_cast<std::uint32_t>(pngs.size()), 2);
    std::uint32_t offset = static_cast<std::uint32_t>(6 + 16 * pngs.size());
    for (std::size_t i = 0; i < pngs.size(); ++i) {
        const auto side = static_cast<std::uint32_t>(kSizes[i] >= 256 ? 0 : kSizes[i]);
        putLittleEndian(ico, side, 1); // width
        putLittleEndian(ico, side, 1); // height
        putLittleEndian(ico, 0, 1);    // no palette
        putLittleEndian(ico, 0, 1);    // reserved
        putLittleEndian(ico, 1, 2);    // colour planes
        putLittleEndian(ico, 32, 2);   // bits per pixel
        putLittleEndian(ico, static_cast<std::uint32_t>(pngs[i].size()), 4);
        putLittleEndian(ico, offset, 4);
        offset += static_cast<std::uint32_t>(pngs[i].size());
    }
    for (const auto& png : pngs) {
        ico.insert(ico.end(), png.begin(), png.end());
    }

    std::ofstream out(std::filesystem::path(argv[2]), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size()));
    if (!out) {
        std::cerr << "make_ico: cannot write the output\n";
        return 1;
    }
    return 0;
}
