#include "image_view.h"

#include <QBrush>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QResizeEvent>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kCheckerSquare = 8;

cv::Point2d toCv(QPointF point) { return {point.x(), point.y()}; }

QPointF toQt(cv::Point2d point) { return {point.x, point.y}; }

// Two greys from the palette, so the checks suit dark and light mode alike.
QBrush checkerboard(const QPalette& palette) {
    QPixmap tile(2 * kCheckerSquare, 2 * kCheckerSquare);
    tile.fill(palette.color(QPalette::Midlight));
    QPainter painter(&tile);
    const QColor dark = palette.color(QPalette::Mid);
    painter.fillRect(kCheckerSquare, 0, kCheckerSquare, kCheckerSquare, dark);
    painter.fillRect(0, kCheckerSquare, kCheckerSquare, kCheckerSquare, dark);
    return QBrush(tile);
}

} // namespace

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true); // pixel readout while hovering, not only while dragging
    setMinimumSize(200, 200);
}

void ImageView::setImage(const QImage& image, KeepView keep, viewer_core::ImageTags::Overlays overlays) {
    setImage(image, keep, image.size(), std::move(overlays));
}

void ImageView::setImage(const QImage& picture, KeepView keep, QSize imageSize,
                         viewer_core::ImageTags::Overlays overlays, QRect pictureArea) {
    const bool sameSize = !m_image.isNull() && imageSize == m_imageSize;
    if (!sameSize) {
        clearRegion(); // its pixels are another image's
    }
    m_image = picture;
    m_imageSize = imageSize;
    const QRect whole(QPoint(0, 0), imageSize);
    m_pictureArea = pictureArea.isEmpty() ? whole : (pictureArea & whole);
    m_overlays = std::move(overlays);
    if (keep == KeepView::No || !sameSize) {
        m_viewport.setImageSize({imageSize.width(), imageSize.height()});
    }
    update();
    // A new image under a resting cursor: report its pixel, or the readout would show the old image's.
    if (m_hoverPosition) {
        reportPixelAt(*m_hoverPosition);
    }
}

void ImageView::clear() {
    clearRegion();
    m_image = QImage();
    m_imageSize = QSize();
    m_pictureArea = QRect();
    m_overlays.reset();
    m_viewport.setImageSize({0, 0});
    update();
}

bool ImageView::hasImage() const { return !m_image.isNull(); }

const std::vector<viewer_core::Overlay>& ImageView::overlays() const {
    static const std::vector<viewer_core::Overlay> kNone;
    return m_overlays ? *m_overlays : kNone;
}

void ImageView::setOverlaysShown(bool shown) {
    if (shown != m_overlaysShown) {
        m_overlaysShown = shown;
        update();
    }
}

bool ImageView::overlaysShown() const { return m_overlaysShown; }

double ImageView::deviceScaleFor(QSize imageSize, KeepView keep) const {
    // The viewport learns the view's size from the first resize, which comes as the view is shown.
    if (!isVisible() || width() <= 0 || height() <= 0 || imageSize.isEmpty()) {
        return 1.0;
    }
    double scale = m_viewport.scale();
    if (keep == KeepView::No || m_image.isNull() || imageSize != m_imageSize) {
        viewer_core::Viewport fitted = m_viewport; // as setImage will fit it
        fitted.setImageSize({imageSize.width(), imageSize.height()});
        scale = fitted.scale();
    }
    return scale > 0.0 ? scale * devicePixelRatioF() : 1.0;
}

const QImage& ImageView::picture() const { return m_image; }

QRect ImageView::pictureArea() const { return m_pictureArea; }

QRect ImageView::visibleAreaFor(QSize imageSize, KeepView keep) const {
    const QRect whole(QPoint(0, 0), imageSize);
    // As deviceScaleFor: before the first resize the viewport has no view to show anything in.
    if (!isVisible() || width() <= 0 || height() <= 0 || keep == KeepView::No || m_image.isNull() ||
        imageSize != m_imageSize) {
        return whole;
    }
    return visibleArea() & whole;
}

QRect ImageView::visibleArea() const {
    if (m_image.isNull()) {
        return {};
    }
    const cv::Rect pixels = m_viewport.visiblePixels();
    return {pixels.x, pixels.y, pixels.width, pixels.height};
}

std::optional<QRect> ImageView::region() const { return m_region; }

void ImageView::setRegionsEnabled(bool enabled) {
    m_regionsEnabled = enabled;
    if (!enabled) {
        clearRegion();
    }
}

bool ImageView::regionsEnabled() const { return m_regionsEnabled; }

// Emits only when there was a region to clear.
void ImageView::clearRegion() {
    m_regionStart.reset();
    m_regionText.clear();
    if (!m_region) {
        return;
    }
    m_region.reset();
    update();
    emit regionChanged(std::nullopt);
}

void ImageView::setRegionText(const QStringList& lines) {
    m_regionText = lines;
    update();
}

const QStringList& ImageView::regionText() const { return m_regionText; }

cv::Point2d ImageView::imagePointAt(QPointF viewPoint) const {
    return m_viewport.viewToImage(toCv(viewPoint));
}

// The pixels from the one under `from` to the one under `to`, both included, within the image.
std::optional<QRect> ImageView::regionBetween(cv::Point2d from, cv::Point2d to) const {
    const int left = static_cast<int>(std::floor(std::min(from.x, to.x)));
    const int top = static_cast<int>(std::floor(std::min(from.y, to.y)));
    const int right = static_cast<int>(std::floor(std::max(from.x, to.x)));
    const int bottom = static_cast<int>(std::floor(std::max(from.y, to.y)));
    const QRect region = QRect(QPoint(left, top), QPoint(right, bottom)) & QRect(QPoint(0, 0), m_imageSize);
    return region.isEmpty() ? std::nullopt : std::optional(region);
}

// The region's label, above the region, or below it when there is no room above; inside the view.
void ImageView::paintRegionLabel(QPainter& painter, const QRectF& outline) const {
    constexpr int kPadding = 4;
    const QString text = m_regionText.join(u'\n');
    const QRect textSize = painter.fontMetrics().boundingRect(QRect(0, 0, width(), height()),
                                                              Qt::AlignLeft | Qt::AlignTop, text);
    const QSize box(textSize.width() + 2 * kPadding, textSize.height() + 2 * kPadding);
    double y = outline.top() - box.height() - 2;
    if (y < 0) {
        y = outline.bottom() + 2;
    }
    const QRectF label(QPointF(std::clamp(outline.left(), 0.0, std::max(0.0, width() - box.width() - 1.0)),
                               std::clamp(y, 0.0, std::max(0.0, height() - box.height() - 1.0))),
                       box);
    painter.setPen(palette().color(QPalette::Highlight));
    painter.setBrush(palette().color(QPalette::ToolTipBase));
    painter.drawRect(label);
    painter.setPen(palette().color(QPalette::ToolTipText));
    painter.drawText(label.adjusted(kPadding, kPadding, -kPadding, -kPadding), Qt::AlignLeft | Qt::AlignTop,
                     text);
}

void ImageView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Read from the palette at paint time, so a switch between dark and light mode is followed.
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (m_image.isNull()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No image yet.\nSend one with visual_debug::sendImage(), stream frames with "
                            "visual_debug::sendLive(), or use File → Open…"));
        return;
    }
    // Only the part of the picture's area in view. Outside the area (a zoomed-in live frame panned past what
    // was converted) the background shows until the next picture covers it.
    const cv::Rect2d area(m_pictureArea.x(), m_pictureArea.y(), m_pictureArea.width(),
                          m_pictureArea.height());
    const cv::Rect2d source = m_viewport.visibleImageRect() & area;
    if (source.width > 0.0 && source.height > 0.0) {
        const QRectF target(toQt(m_viewport.imageToView(source.tl())),
                            toQt(m_viewport.imageToView(source.br())));
        // Under the image, so transparent pixels show as checks. Opaque images cover it completely. The
        // checks start at the image's corner, so they move with it when panning.
        painter.setBrushOrigin(toQt(m_viewport.imageToView({0.0, 0.0})));
        painter.fillRect(target, checkerboard(palette()));
        // The picture may be smaller than its area: its pixels per image pixel, across and down, each
        // rounded its own way when it was made.
        const double scaleX = m_image.width() / area.width;
        const double scaleY = m_image.height() / area.height;
        // Smooth when shrinking; nearest-neighbour when enlarging, so individual pixels stay visible. A
        // picture made at the drawn size is copied about 1:1, where smoothing would only cost.
        const double devicePerPicturePixel = m_viewport.scale() * devicePixelRatioF() / scaleX;
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_viewport.scale() < 1.0 && std::abs(devicePerPicturePixel - 1.0) > 0.01);
        painter.drawImage(target, m_image,
                          QRectF((source.x - area.x) * scaleX, (source.y - area.y) * scaleY,
                                 source.width * scaleX, source.height * scaleY));
    }
    if (m_overlaysShown && m_overlays) {
        // Image pixels, not the picture's, which may be smaller.
        const QPointF origin = toQt(m_viewport.imageToView({0.0, 0.0}));
        const double scale = m_viewport.scale();
        m_overlayPainter.paint(painter, *m_overlays, QTransform(scale, 0, 0, scale, origin.x(), origin.y()));
    }
    if (m_region) {
        const QRectF outline(
            toQt(m_viewport.imageToView(
                {static_cast<double>(m_region->left()), static_cast<double>(m_region->top())})),
            toQt(m_viewport.imageToView({static_cast<double>(m_region->left() + m_region->width()),
                                         static_cast<double>(m_region->top() + m_region->height())})));
        QPen pen(palette().color(QPalette::Highlight));
        pen.setCosmetic(true);
        pen.setWidthF(1.0 / devicePixelRatioF()); // one device pixel, at any zoom
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(outline);
        if (!m_regionText.isEmpty()) {
            paintRegionLabel(painter, outline);
        }
    }
}

void ImageView::resizeEvent(QResizeEvent* event) {
    m_viewport.setViewSize({static_cast<double>(width()), static_cast<double>(height())});
    QWidget::resizeEvent(event);
    emit scaleChanged();
}

void ImageView::wheelEvent(QWheelEvent* event) {
    if (m_image.isNull()) {
        return;
    }
    // 120 units is one notch of a mouse wheel; trackpads send smaller deltas, which zoom proportionally.
    m_viewport.zoomAt(toCv(event->position()), event->angleDelta().y() / 120.0);
    update();
    // Zoom keeps the point under the cursor, except where it stops at the fit and re-centres the image.
    reportPixelAt(event->position());
    event->accept();
    emit scaleChanged();
}

// Shift + drag draws a region and never pans; a plain drag pans.
void ImageView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || m_image.isNull()) {
        return;
    }
    m_pressPosition = event->position();
    if (m_regionsEnabled && event->modifiers().testFlag(Qt::ShiftModifier)) {
        m_regionStart = imagePointAt(event->position());
        m_regionText.clear();
        m_region = regionBetween(*m_regionStart, *m_regionStart);
        update();
        return;
    }
    m_lastDragPosition = event->position();
    setCursor(Qt::ClosedHandCursor);
}

void ImageView::mouseMoveEvent(QMouseEvent* event) {
    m_hoverPosition = event->position();
    if (m_regionStart) {
        m_region = regionBetween(*m_regionStart, imagePointAt(event->position()));
        update();
    } else if (m_lastDragPosition) {
        m_viewport.panBy(toCv(event->position() - *m_lastDragPosition));
        m_lastDragPosition = event->position();
        update();
        emit panned();
    }
    reportPixelAt(event->position());
}

void ImageView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (m_regionStart) {
        m_region = regionBetween(*m_regionStart, imagePointAt(event->position()));
        m_regionStart.reset();
        update();
        emit regionChanged(m_region);
        return;
    }
    // A click that did not move clears the region; a pan keeps it.
    constexpr double kClickSlop = 3.0;
    const bool clicked =
        m_pressPosition && (event->position() - *m_pressPosition).manhattanLength() < kClickSlop;
    m_lastDragPosition.reset();
    m_pressPosition.reset();
    unsetCursor();
    if (clicked) {
        clearRegion();
    }
}

void ImageView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_viewport.fit();
        update();
        reportPixelAt(event->position()); // fitting moves the image under the cursor
        emit scaleChanged();
    }
}

void ImageView::leaveEvent(QEvent* event) {
    m_hoverPosition.reset();
    emit hoverLeft();
    QWidget::leaveEvent(event);
}

void ImageView::reportPixelAt(QPointF viewPoint) {
    if (m_image.isNull()) {
        return;
    }
    // Floor, not truncation: the point at -0.5 lies in pixel -1 (outside the image), not in pixel 0.
    const cv::Point2d imagePoint = m_viewport.viewToImage(toCv(viewPoint));
    emit pixelHovered(static_cast<int>(std::floor(imagePoint.x)), static_cast<int>(std::floor(imagePoint.y)));
}
