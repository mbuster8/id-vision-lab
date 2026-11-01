#include "image_view.h"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPalette>
#include <QRect>
#include <QSignalSpy>
#include <QStringList>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include <optional>

namespace {

QImage filledImage(int width, int height) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::gray);
    return image;
}

void sendMouseEvent(ImageView& view, QEvent::Type type, QPointF position, Qt::MouseButton button,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, view.mapToGlobal(position), button, button, modifiers);
    QCoreApplication::sendEvent(&view, &event);
}

// A left-button drag from `from` to `to`, in view coordinates.
void drag(ImageView& view, QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    sendMouseEvent(view, QEvent::MouseButtonPress, from, Qt::LeftButton, modifiers);
    sendMouseEvent(view, QEvent::MouseMove, to, Qt::LeftButton, modifiers);
    sendMouseEvent(view, QEvent::MouseButtonRelease, to, Qt::LeftButton, modifiers);
}

// 120 units is one notch of a mouse wheel; positive zooms in.
void sendWheelEvent(ImageView& view, QPointF position, int angleDelta) {
    QWheelEvent event(position, view.mapToGlobal(position), QPoint(), QPoint(0, angleDelta), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&view, &event);
}

// A 200×200 view: a 100×100 image fits at scale 2, a 400×400 one at scale 0.5.
class ImageViewTest : public testing::Test {
protected:
    void SetUp() override {
        m_view.resize(200, 200);
        m_view.show();
        m_view.setImage(filledImage(100, 100));
    }

    ImageView m_view;
};

// --- Regions (the 100×100 image fills the 200×200 view at scale 2) ---------------------------------------

// From the pixel under the press to the pixel under the release, both included; Shift + drag never pans.
TEST_F(ImageViewTest, ShiftDragDrawsARegion) {
    QSignalSpy changed(&m_view, &ImageView::regionChanged);
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);

    drag(m_view, {20, 40}, {60, 100}, Qt::ShiftModifier);

    ASSERT_EQ(changed.size(), 1);
    EXPECT_EQ(m_view.region(), std::optional(QRect(10, 20, 21, 31)));
    sendMouseEvent(m_view, QEvent::MouseMove, {20, 40}, Qt::NoButton);
    EXPECT_EQ(hovered.last().at(0).toInt(), 10); // not panned
    EXPECT_EQ(hovered.last().at(1).toInt(), 20);
}

TEST_F(ImageViewTest, RegionIsClippedToTheImage) {
    m_view.setImage(filledImage(50, 50)); // at scale 4

    drag(m_view, {-40, -40}, {120, 160}, Qt::ShiftModifier);

    EXPECT_EQ(m_view.region(), std::optional(QRect(0, 0, 31, 41)));
}

TEST_F(ImageViewTest, SameSizeKeepsTheRegion) {
    drag(m_view, {20, 40}, {60, 100}, Qt::ShiftModifier);
    QSignalSpy changed(&m_view, &ImageView::regionChanged);

    m_view.setImage(filledImage(100, 100)); // another image of the same size
    EXPECT_EQ(m_view.region(), std::optional(QRect(10, 20, 21, 31)));
    EXPECT_EQ(changed.size(), 0);

    m_view.setImage(filledImage(50, 50));
    EXPECT_EQ(m_view.region(), std::nullopt);
    EXPECT_EQ(changed.size(), 1);
}

TEST_F(ImageViewTest, ClickClearsTheRegion) {
    drag(m_view, {20, 40}, {60, 100}, Qt::ShiftModifier);
    m_view.setRegionText({QStringLiteral("21×31 at 10, 20")});
    drag(m_view, {100, 100}, {150, 150}); // a pan keeps it
    EXPECT_TRUE(m_view.region().has_value());
    QSignalSpy changed(&m_view, &ImageView::regionChanged);

    drag(m_view, {100, 100}, {100, 100});

    EXPECT_EQ(m_view.region(), std::nullopt);
    EXPECT_TRUE(m_view.regionText().isEmpty());
    EXPECT_EQ(changed.size(), 1);
}

TEST_F(ImageViewTest, ReportsPixelUnderRestingCursorWhenImageChanges) {
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);
    sendMouseEvent(m_view, QEvent::MouseMove, {100, 100}, Qt::NoButton);
    ASSERT_EQ(hovered.size(), 1);

    m_view.setImage(filledImage(400, 400)); // a new image arrives; the cursor has not moved

    ASSERT_EQ(hovered.size(), 2);
    EXPECT_EQ(hovered.last().at(0).toInt(), 200); // the centre of the new image, not the old (50, 50)
    EXPECT_EQ(hovered.last().at(1).toInt(), 200);
}

TEST_F(ImageViewTest, ReportsPixelUnderCursorAfterFitting) {
    sendMouseEvent(m_view, QEvent::MouseMove, {150, 150}, Qt::NoButton);
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);

    sendMouseEvent(m_view, QEvent::MouseButtonDblClick, {150, 150}, Qt::LeftButton);

    EXPECT_EQ(hovered.size(), 1);
}

TEST_F(ImageViewTest, ReportsPixelUnderCursorAfterZoomingOutToFit) {
    sendWheelEvent(m_view, {20, 20}, 4 * 120); // zoomed in around image point (10, 10)
    sendMouseEvent(m_view, QEvent::MouseMove, {180, 180}, Qt::NoButton); // over image pixel (42, 42)
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);

    // Zooming out stops at the fit, which centres the image, so a different pixel ends up under the cursor.
    sendWheelEvent(m_view, {180, 180}, -10 * 120);

    ASSERT_EQ(hovered.size(), 1);
    EXPECT_EQ(hovered.last().at(0).toInt(), 90);
    EXPECT_EQ(hovered.last().at(1).toInt(), 90);
}

// A new picture of the same image (another display range, the next frame) keeps the zoom and pan.
TEST_F(ImageViewTest, KeepViewHoldsTheZoomForTheSameSize) {
    sendWheelEvent(m_view, {20, 20}, 4 * 120);
    sendMouseEvent(m_view, QEvent::MouseMove, {180, 180}, Qt::NoButton); // over image pixel (42, 42)
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);

    m_view.setImage(filledImage(100, 100), ImageView::KeepView::IfSameSize);
    ASSERT_EQ(hovered.size(), 1);
    EXPECT_EQ(hovered.last().at(0).toInt(), 42);

    m_view.setImage(filledImage(100, 100)); // an image of its own is fitted
    ASSERT_EQ(hovered.size(), 2);
    EXPECT_EQ(hovered.last().at(0).toInt(), 90);
}

TEST_F(ImageViewTest, KeepViewRefitsANewSize) {
    sendWheelEvent(m_view, {20, 20}, 4 * 120);
    sendMouseEvent(m_view, QEvent::MouseMove, {100, 100}, Qt::NoButton);
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);

    m_view.setImage(filledImage(400, 400), ImageView::KeepView::IfSameSize);

    ASSERT_EQ(hovered.size(), 1);
    EXPECT_EQ(hovered.last().at(0).toInt(), 200); // fitted: the centre of the new image
    EXPECT_EQ(hovered.last().at(1).toInt(), 200);
}

// (F10) A picture made smaller than the image it stands for: zoom, pan and the readout use the image's size.
TEST_F(ImageViewTest, PictureStandsForTheImageSize) {
    m_view.setImage(filledImage(100, 75), ImageView::KeepView::No, QSize(400, 300));
    QSignalSpy hovered(&m_view, &ImageView::pixelHovered);

    sendMouseEvent(m_view, QEvent::MouseMove, {100, 100}, Qt::NoButton); // the centre of the fitted image

    ASSERT_EQ(hovered.size(), 1);
    EXPECT_NEAR(hovered.last().at(0).toInt(), 200, 2);
    EXPECT_NEAR(hovered.last().at(1).toInt(), 150, 2);
    EXPECT_EQ(m_view.picture().size(), QSize(100, 75));
}

// The scale the next picture will be drawn at, so the window can make it at that size.
TEST_F(ImageViewTest, DeviceScaleForTellsTheScaleToCome) {
    const double ratio = m_view.devicePixelRatioF();
    EXPECT_DOUBLE_EQ(m_view.deviceScaleFor(QSize(100, 100), ImageView::KeepView::IfSameSize), 2.0 * ratio);
    EXPECT_DOUBLE_EQ(m_view.deviceScaleFor(QSize(400, 400), ImageView::KeepView::IfSameSize), 0.5 * ratio);

    sendWheelEvent(m_view, {100, 100}, 4 * 120);

    EXPECT_GT(m_view.deviceScaleFor(QSize(100, 100), ImageView::KeepView::IfSameSize), 2.0 * ratio);
    EXPECT_DOUBLE_EQ(m_view.deviceScaleFor(QSize(100, 100), ImageView::KeepView::No), 2.0 * ratio);
}

// Before the view has a size there is no scale to make a picture for: the full size is asked for.
TEST(ImageViewWithoutSize, DeviceScaleForIsOneBeforeTheViewIsShown) {
    ImageView view;
    EXPECT_DOUBLE_EQ(view.deviceScaleFor(QSize(4000, 3000), ImageView::KeepView::No), 1.0);
}

// The window redraws a live frame when the scale changes: on a zoom, a fit or a resize, never on a new
// picture.
TEST_F(ImageViewTest, ScaleChangesOnZoomFitAndResizeOnly) {
    QSignalSpy changed(&m_view, &ImageView::scaleChanged);

    m_view.setImage(filledImage(100, 100), ImageView::KeepView::IfSameSize);
    EXPECT_EQ(changed.size(), 0);
    sendWheelEvent(m_view, {100, 100}, 120);
    EXPECT_EQ(changed.size(), 1);
    sendMouseEvent(m_view, QEvent::MouseButtonDblClick, {100, 100}, Qt::LeftButton);
    EXPECT_EQ(changed.size(), 2);
    m_view.resize(300, 300);
    EXPECT_EQ(changed.size(), 3);
}

// The view's pixel at (x, y) in widget coordinates.
QColor pixelOf(ImageView& view, int x, int y) {
    const QImage grabbed = view.grab().toImage();
    const qreal ratio = grabbed.devicePixelRatio();
    return grabbed.pixelColor(qRound(x * ratio), qRound(y * ratio));
}

// A 100×50 image fits the 200×200 view at scale 2: 200×100, centred, so rows 0 to 49 are background.
TEST_F(ImageViewTest, BackgroundIsTheWindowColour) {
    m_view.setImage(filledImage(100, 50));

    EXPECT_EQ(pixelOf(m_view, 100, 10).rgb(), m_view.palette().color(QPalette::Window).rgb());
}

TEST_F(ImageViewTest, TransparentPixelsShowACheckerboard) {
    QImage transparent(100, 100, QImage::Format_RGBA8888);
    transparent.fill(Qt::transparent);
    m_view.setImage(transparent);

    const QColor first = pixelOf(m_view, 4, 4); // checker squares are 8 px, from the image's corner
    const QColor second = pixelOf(m_view, 12, 4);
    const QColor window = m_view.palette().color(QPalette::Window);

    EXPECT_NE(first.rgb(), second.rgb());
    EXPECT_NE(first.rgb(), window.rgb());
    EXPECT_NE(second.rgb(), window.rgb());
}

TEST_F(ImageViewTest, BackgroundFollowsPaletteChange) {
    m_view.setImage(filledImage(100, 50));
    QPalette palette = m_view.palette();
    palette.setColor(QPalette::Window, Qt::green);

    m_view.setPalette(palette);
    m_view.update();

    EXPECT_EQ(pixelOf(m_view, 100, 10).rgb(), QColor(Qt::green).rgb());
}

} // namespace
