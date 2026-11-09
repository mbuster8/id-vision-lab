#pragma once

#include "overlay_painter.h"

#include "viewer_core/image_tags.h"
#include "viewer_core/viewport.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <vector>

class QPainter;

// Shows one image: wheel to zoom at the cursor, drag to pan, double-click to fit. Reports the image pixel
// under the cursor, so the window can show its original values.
class ImageView : public QWidget {
    Q_OBJECT

public:
    // IfSameSize keeps the zoom and pan for a new picture of the same size: another display range of the
    // shown image, or a live channel's next frame. Any other picture is fitted to the view.
    enum class KeepView { No, IfSameSize };

    explicit ImageView(QWidget* parent = nullptr);

    // Each picture comes with the overlays drawn over it, in image pixels; a picture set without them shows
    // none, so a caller that forgets them shows too little rather than another picture's.
    void setImage(const QImage& image, KeepView keep = KeepView::No,
                  viewer_core::ImageTags::Overlays overlays = {});
    // A picture standing for an image of imageSize pixels, made smaller to be drawn at the view's scale:
    // zoom, pan, the pixel readout and the overlays use imageSize, and the picture is stretched over it. With
    // a pictureArea, in image pixels, the picture is of that part only, stretched over it; the rest of the
    // image shows the background. Empty: the whole image.
    void setImage(const QImage& picture, KeepView keep, QSize imageSize,
                  viewer_core::ImageTags::Overlays overlays = {}, QRect pictureArea = {});
    void clear();
    bool hasImage() const;
    // Device pixels per image pixel that a picture of an image this size, set with `keep`, will be drawn at;
    // 1 while the view has no size yet.
    double deviceScaleFor(QSize imageSize, KeepView keep) const;
    const QImage& picture() const; // as set; null for none
    QRect pictureArea() const;     // the part of the image the picture covers; empty for none
    // Every image pixel at least partly in view, in image pixels: the whole image when fitted; empty for
    // none.
    QRect visibleArea() const;
    // The same for a picture of an image this size, set with `keep`: the whole image when it will be fitted,
    // and while the view has no size yet.
    QRect visibleAreaFor(QSize imageSize, KeepView keep) const;
    const std::vector<viewer_core::Overlay>& overlays() const; // empty for none
    // Shown at construction. Hiding them only repaints; the picture is not made again.
    void setOverlaysShown(bool shown);
    bool overlaysShown() const;

    // A region of the image drawn with Shift + drag, in image pixels, clipped to the image. A new picture of
    // the same image size keeps it; another size, or a left click without a drag, clears it.
    std::optional<QRect> region() const;
    void clearRegion();
    // The lines of the region's label, drawn in a box above it (below when there is no room above).
    void setRegionText(const QStringList& lines);
    const QStringList& regionText() const;

signals:
    void pixelHovered(int x, int y);
    void hoverLeft();
    void scaleChanged(); // after a zoom, a fit or a resize; never for a new picture
    void panned();       // after a drag moved the image; the scale is unchanged
    void regionChanged(std::optional<QRect> region);

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
    cv::Point2d imagePointAt(QPointF viewPoint) const;
    std::optional<QRect> regionBetween(cv::Point2d from, cv::Point2d to) const;
    void paintRegionLabel(QPainter& painter, const QRectF& outline) const;

    QImage m_image;
    QSize m_imageSize;   // what m_image stands for; its own size unless it was made smaller
    QRect m_pictureArea; // the part of it m_image covers: all of it unless a part was converted
    viewer_core::Viewport m_viewport;
    std::optional<QPointF> m_lastDragPosition;
    std::optional<QPointF> m_hoverPosition;   // while the cursor is over the view
    std::optional<QPointF> m_pressPosition;   // of the left button, to tell a click from a drag
    std::optional<cv::Point2d> m_regionStart; // image point, while a Shift + drag draws a region
    std::optional<QRect> m_region;
    QStringList m_regionText;
    viewer_core::ImageTags::Overlays m_overlays;
    qt_common::OverlayPainter m_overlayPainter; // keeps the labels laid out between paints
    bool m_overlaysShown = true;
};
