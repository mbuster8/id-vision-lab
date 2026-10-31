#pragma once

#include "viewer_core/viewport.h"

#include <QImage>
#include <QPointF>
#include <QWidget>

#include <optional>

// Shows one image: wheel to zoom at the cursor, drag to pan, double-click to fit. Reports the image pixel
// under the cursor, so the window can show its original values.
class ImageView : public QWidget {
    Q_OBJECT

public:
    // IfSameSize keeps the zoom and pan for a new picture of the same size: another display range of the
    // shown image, or a live channel's next frame. Any other picture is fitted to the view.
    enum class KeepView { No, IfSameSize };

    explicit ImageView(QWidget* parent = nullptr);

    void setImage(const QImage& image, KeepView keep = KeepView::No);
    // A picture standing for an image of imageSize pixels, made smaller to be drawn at the view's scale:
    // zoom, pan and the pixel readout use imageSize, and the picture is stretched over it.
    void setImage(const QImage& picture, KeepView keep, QSize imageSize);
    void clear();
    bool hasImage() const;
    // Device pixels per image pixel that a picture of an image this size, set with `keep`, will be drawn at;
    // 1 while the view has no size yet.
    double deviceScaleFor(QSize imageSize, KeepView keep) const;
    const QImage& picture() const; // as set; null for none

signals:
    void pixelHovered(int x, int y);
    void hoverLeft();
    void scaleChanged(); // after a zoom, a fit or a resize; never for a new picture

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void reportPixelAt(QPointF viewPoint);

    QImage m_image;
    QSize m_imageSize; // what m_image stands for; its own size unless it was made smaller
    viewer_core::Viewport m_viewport;
    std::optional<QPointF> m_lastDragPosition;
    std::optional<QPointF> m_hoverPosition; // while the cursor is over the view
};
