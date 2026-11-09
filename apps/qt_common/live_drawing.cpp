#include "live_drawing.h"

#include "viewer_core/display_converter.h"

#include <QRect>
#include <QSize>

#include <cmath>

cv::Size drawnSizeOf(cv::Size part, double scale) {
    if (scale >= 1.0) {
        return part;
    }
    return viewer_core::sizeWithin(part, cv::Size(static_cast<int>(std::ceil(part.width * scale)),
                                                  static_cast<int>(std::ceil(part.height * scale))));
}

LiveDrawing liveDrawing(const ImageView& view, cv::Size frameSize, ImageView::KeepView keep) {
    constexpr double kMostOfTheFrame = 0.75;
    const QSize imageSize(frameSize.width, frameSize.height);
    const cv::Rect whole(0, 0, frameSize.width, frameSize.height);
    cv::Rect area = whole;
    if (const QRect visible = view.visibleAreaFor(imageSize, keep); !visible.isEmpty()) {
        const int marginX = visible.width() / 4;
        const int marginY = visible.height() / 4;
        const cv::Rect cut = cv::Rect(visible.x() - marginX, visible.y() - marginY,
                                      visible.width() + 2 * marginX, visible.height() + 2 * marginY) &
                             whole;
        if (!cut.empty() &&
            static_cast<double>(cut.area()) < kMostOfTheFrame * static_cast<double>(whole.area())) {
            area = cut;
        }
    }
    return {area, drawnSizeOf(area.size(), view.deviceScaleFor(imageSize, keep))};
}

bool needsRedraw(const ImageView& view, cv::Size frameSize, const std::optional<LiveDrawing>& newest) {
    if (!newest) {
        return true;
    }
    const QSize imageSize(frameSize.width, frameSize.height);
    const QRect visible = view.visibleAreaFor(imageSize, ImageView::KeepView::IfSameSize);
    const double scale = view.deviceScaleFor(imageSize, ImageView::KeepView::IfSameSize);
    const bool covered =
        QRect(newest->area.x, newest->area.y, newest->area.width, newest->area.height).contains(visible);
    return !covered || newest->size != drawnSizeOf(newest->area.size(), scale);
}
