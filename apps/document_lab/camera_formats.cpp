#include "camera_formats.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace document_lab {

QString CameraFormat::label() const {
    return QStringLiteral("%1×%2, %3 fps, %4")
        .arg(size.width())
        .arg(size.height())
        .arg(QString::number(static_cast<double>(maxFps), 'g', 4))
        .arg(QVideoFrameFormat::pixelFormatToString(pixelFormat));
}

namespace {

// The fastest a camera starts at by default: more costs processing and recording for little a document needs.
constexpr float kHighestRate = 60.0F;

int pixelsOf(const CameraFormat& format) { return format.size.width() * format.size.height(); }

// Most pixels first, then the highest rate, then by pixel format: the order the picker lists them in.
bool listedBefore(const CameraFormat& a, const CameraFormat& b) {
    return std::make_tuple(-pixelsOf(a), -a.maxFps, static_cast<int>(a.pixelFormat)) <
           std::make_tuple(-pixelsOf(b), -b.maxFps, static_cast<int>(b.pixelFormat));
}

} // namespace

std::vector<CameraFormat> distinctFormats(std::vector<CameraFormat> formats) {
    std::sort(formats.begin(), formats.end(), listedBefore);
    formats.erase(std::unique(formats.begin(), formats.end()), formats.end());
    return formats;
}

std::optional<CameraFormat> defaultFormat(const std::vector<CameraFormat>& formats) {
    const auto rank = [](const CameraFormat& format) {
        const bool compressed = format.pixelFormat == QVideoFrameFormat::Format_Jpeg;
        const bool tooFast = format.maxFps > kHighestRate + 0.5F;
        // Smaller is better: uncompressed, most pixels, at most 60 fps and then the fastest, then NV12.
        return std::make_tuple(compressed, -pixelsOf(format), tooFast,
                               tooFast ? format.maxFps : -format.maxFps,
                               format.pixelFormat != QVideoFrameFormat::Format_NV12);
    };
    const auto best =
        std::min_element(formats.begin(), formats.end(),
                         [&rank](const CameraFormat& a, const CameraFormat& b) { return rank(a) < rank(b); });
    if (best == formats.end()) {
        return std::nullopt;
    }
    return *best;
}

} // namespace document_lab
