#include "image_view.h"

#include <QBrush>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QWheelEvent>

#include <cmath>

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

void ImageView::setImage(const QImage& image, KeepView keep) { setImage(image, keep, image.size()); }

void ImageView::setImage(const QImage& picture, KeepView keep, QSize imageSize) {
    const bool sameSize = !m_image.isNull() && imageSize == m_imageSize;
    m_image = picture;
    m_imageSize = imageSize;
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
    m_image = QImage();
    m_imageSize = QSize();
    m_viewport.setImageSize({0, 0});
    update();
}

bool ImageView::hasImage() const { return !m_image.isNull(); }

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
    const cv::Rect2d source = m_viewport.visibleImageRect();
    const QRectF target(toQt(m_viewport.imageToView(source.tl())), toQt(m_viewport.imageToView(source.br())));
    // Under the image, so transparent pixels show as checks. Opaque images cover it completely. The checks
    // start at the image's corner, so they move with it when panning.
    painter.setBrushOrigin(toQt(m_viewport.imageToView({0.0, 0.0})));
    painter.fillRect(target, checkerboard(palette()));
    // The picture may be smaller than the image it stands for: its pixels per image pixel.
    const double pictureScale = static_cast<double>(m_image.width()) / m_imageSize.width();
    // Smooth when shrinking; nearest-neighbour when enlarging, so individual pixels stay visible. A picture
    // made at the drawn size is copied about 1:1, where smoothing would only cost.
    const double devicePerPicturePixel = m_viewport.scale() * devicePixelRatioF() / pictureScale;
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          m_viewport.scale() < 1.0 && std::abs(devicePerPicturePixel - 1.0) > 0.01);
    painter.drawImage(target, m_image,
                      QRectF(source.x * pictureScale, source.y * pictureScale, source.width * pictureScale,
                             source.height * pictureScale));
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

void ImageView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !m_image.isNull()) {
        m_lastDragPosition = event->position();
        setCursor(Qt::ClosedHandCursor);
    }
}

void ImageView::mouseMoveEvent(QMouseEvent* event) {
    m_hoverPosition = event->position();
    if (m_lastDragPosition) {
        m_viewport.panBy(toCv(event->position() - *m_lastDragPosition));
        m_lastDragPosition = event->position();
        update();
    }
    reportPixelAt(event->position());
}

void ImageView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_lastDragPosition.reset();
        unsetCursor();
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
