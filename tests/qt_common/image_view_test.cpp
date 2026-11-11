#include "image_tags_conversion.h"
#include "image_view.h"

#include "visual_debug/wire_format.h"

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPalette>
#include <QRect>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

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

// --- A single click (A3.3: click-to-focus)
// -------------------------------------------------------------------

// A click that did not move reports its image point once the double-click interval has passed.
TEST_F(ImageViewTest, ClickEmitsTheImagePointOnce) {
    m_view.setClickDelay(10);
    QSignalSpy clicked(&m_view, &ImageView::clicked);

    sendMouseEvent(m_view, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton);
    sendMouseEvent(m_view, QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton);
    EXPECT_EQ(clicked.size(), 0); // not before the delay
    // Waits for the signal, not for a fixed time: a slow first paint (fonts, on a busy runner) can use up a
    // fixed wait before the click's timer is seen.
    EXPECT_TRUE(clicked.wait(5000));

    ASSERT_EQ(clicked.size(), 1);
    const QPointF point = clicked.first().at(0).toPointF();
    EXPECT_NEAR(point.x(), 50.0, 1.0);
    EXPECT_NEAR(point.y(), 50.0, 1.0);
}

// A real double-click arrives as press, release, double-click, release: it fits the view and clicks nothing.
TEST_F(ImageViewTest, DoubleClickEmitsNothing) {
    m_view.setClickDelay(10);
    QSignalSpy clicked(&m_view, &ImageView::clicked);
    QSignalSpy scaled(&m_view, &ImageView::scaleChanged);

    sendMouseEvent(m_view, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton);
    sendMouseEvent(m_view, QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton);
    sendMouseEvent(m_view, QEvent::MouseButtonDblClick, {100, 100}, Qt::LeftButton);
    sendMouseEvent(m_view, QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton);
    QTest::qWait(50);

    EXPECT_EQ(clicked.size(), 0);
    EXPECT_EQ(scaled.size(), 1);
}

TEST_F(ImageViewTest, DragEmitsNothing) {
    m_view.setClickDelay(10);
    QSignalSpy clicked(&m_view, &ImageView::clicked);

    drag(m_view, {20, 40}, {60, 100});
    QTest::qWait(50);

    EXPECT_EQ(clicked.size(), 0);
}

// The letterbox around a wide image is not the image.
TEST_F(ImageViewTest, ClickOutsideTheImageEmitsNothing) {
    m_view.setImage(filledImage(100, 50)); // 200 × 100 at scale 2, from y 50 to 150
    m_view.setClickDelay(10);
    QSignalSpy clicked(&m_view, &ImageView::clicked);

    sendMouseEvent(m_view, QEvent::MouseButtonPress, {100, 20}, Qt::LeftButton);
    sendMouseEvent(m_view, QEvent::MouseButtonRelease, {100, 20}, Qt::LeftButton);
    QTest::qWait(50);

    EXPECT_EQ(clicked.size(), 0);
}

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

// A window with no use for regions, such as the Document Lab's, switches them off: Shift + drag then pans,
// and a region already drawn goes.
TEST_F(ImageViewTest, RegionsCanBeSwitchedOff) {
    EXPECT_TRUE(m_view.regionsEnabled());
    drag(m_view, {20, 40}, {60, 100}, Qt::ShiftModifier);
    QSignalSpy changed(&m_view, &ImageView::regionChanged);
    QSignalSpy panned(&m_view, &ImageView::panned);

    m_view.setRegionsEnabled(false);
    EXPECT_FALSE(m_view.regionsEnabled());
    EXPECT_EQ(m_view.region(), std::nullopt);
    EXPECT_EQ(changed.size(), 1);

    sendWheelEvent(m_view, {100, 100}, 4 * 120); // zoomed in, so a drag can pan
    changed.clear();
    drag(m_view, {20, 40}, {60, 100}, Qt::ShiftModifier);

    EXPECT_EQ(m_view.region(), std::nullopt);
    EXPECT_EQ(changed.size(), 0);
    EXPECT_EQ(panned.size(), 1);
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

// A pan moves what is in view without changing the scale: the window converts a zoomed-in live frame's area
// in view, so it is told. Hovering and Shift + drag do not pan.
TEST_F(ImageViewTest, PanningIsReported) {
    sendWheelEvent(m_view, {100, 100}, 4 * 120);
    QSignalSpy panned(&m_view, &ImageView::panned);

    sendMouseEvent(m_view, QEvent::MouseMove, {50, 50}, Qt::NoButton);
    drag(m_view, {20, 40}, {60, 100}, Qt::ShiftModifier);
    EXPECT_EQ(panned.size(), 0);

    drag(m_view, {100, 100}, {150, 120});
    EXPECT_EQ(panned.size(), 1);
}

// The image pixels a picture set with `keep` will have in view, so the window can convert only those.
TEST_F(ImageViewTest, VisibleAreaForTellsTheAreaToCome) {
    EXPECT_EQ(m_view.visibleAreaFor(QSize(100, 100), ImageView::KeepView::IfSameSize), QRect(0, 0, 100, 100));

    sendWheelEvent(m_view, {100, 100}, 4 * 120);

    const QRect zoomed = m_view.visibleAreaFor(QSize(100, 100), ImageView::KeepView::IfSameSize);
    EXPECT_EQ(zoomed, m_view.visibleArea());
    EXPECT_LT(zoomed.width(), 100);
    EXPECT_TRUE(QRect(0, 0, 100, 100).contains(zoomed));
    // Fitted: a picture set without keeping the view, or for another size.
    EXPECT_EQ(m_view.visibleAreaFor(QSize(100, 100), ImageView::KeepView::No), QRect(0, 0, 100, 100));
    EXPECT_EQ(m_view.visibleAreaFor(QSize(400, 300), ImageView::KeepView::IfSameSize), QRect(0, 0, 400, 300));
}

TEST(ImageViewWithoutSize, VisibleAreaForIsTheWholeImageBeforeTheViewIsShown) {
    ImageView view;
    EXPECT_EQ(view.visibleAreaFor(QSize(4000, 3000), ImageView::KeepView::IfSameSize),
              QRect(0, 0, 4000, 3000));
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

// The viewer says how to send an image; another program says what fits it.
TEST_F(ImageViewTest, EmptyTextCanBeChanged) {
    ImageView view;
    EXPECT_TRUE(view.emptyText().contains(QStringLiteral("visual_debug::sendImage()")));

    view.setEmptyText(QStringLiteral("Start a camera."));

    EXPECT_EQ(view.emptyText(), QStringLiteral("Start a camera."));
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

// A zoomed-in live frame's picture covers only the area converted: its picture is stretched over that area of
// the image, each side by its own factor, and the rest shows the background until the next picture.
TEST_F(ImageViewTest, APictureOfPartOfTheImageIsDrawnOverThatPart) {
    QImage picture(2, 2, QImage::Format_RGBA8888);
    picture.setPixelColor(0, 0, Qt::red);
    picture.setPixelColor(1, 0, Qt::blue);
    picture.setPixelColor(0, 1, Qt::green);
    picture.setPixelColor(1, 1, Qt::yellow);

    // The top half of the 100×100 image, 100×50 image pixels: 50 per picture pixel across, 25 down.
    m_view.setImage(picture, ImageView::KeepView::No, QSize(100, 100), {}, QRect(0, 0, 100, 50));

    EXPECT_EQ(m_view.pictureArea(), QRect(0, 0, 100, 50));
    EXPECT_EQ(pixelOf(m_view, 50, 30).rgb(), QColor(Qt::red).rgb());     // image (25, 15)
    EXPECT_EQ(pixelOf(m_view, 150, 30).rgb(), QColor(Qt::blue).rgb());   // image (75, 15)
    EXPECT_EQ(pixelOf(m_view, 50, 70).rgb(), QColor(Qt::green).rgb());   // image (25, 35)
    EXPECT_EQ(pixelOf(m_view, 150, 70).rgb(), QColor(Qt::yellow).rgb()); // image (75, 35)
    EXPECT_EQ(pixelOf(m_view, 100, 150).rgb(), m_view.palette().color(QPalette::Window).rgb());
}

TEST_F(ImageViewTest, APictureSetWithoutAnAreaCoversTheWholeImage) {
    m_view.setImage(filledImage(50, 50), ImageView::KeepView::IfSameSize, QSize(100, 100), {},
                    QRect(50, 50, 50, 50));

    m_view.setImage(filledImage(100, 100), ImageView::KeepView::IfSameSize);

    EXPECT_EQ(m_view.pictureArea(), QRect(0, 0, 100, 100));
    EXPECT_EQ(pixelOf(m_view, 50, 50).rgb(), QColor(Qt::gray).rgb());
}

// --- Overlays (a 100×100 black image fills the 200×200 view at scale 2) ----------------------------------

QImage blackImage(int width, int height) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::black);
    return image;
}

viewer_core::ImageTags::Overlays leftHalf() {
    return viewer_core::makeOverlays({viewer_core::Rectangle{{0, 0, 50, 100}}});
}

bool isBlack(const QColor& colour) { return colour.red() + colour.green() + colour.blue() < 10; }

TEST_F(ImageViewTest, DrawsOverlaysAboveTheImage) {
    m_view.setImage(blackImage(100, 100), ImageView::KeepView::No, leftHalf());

    EXPECT_EQ(m_view.overlays().size(), 1U);
    EXPECT_FALSE(isBlack(pixelOf(m_view, 50, 100)));
    EXPECT_TRUE(isBlack(pixelOf(m_view, 150, 100)));
}

// Green dominates an overlay's edge, which no black image pixel has.
bool isEdge(const QColor& colour) { return colour.green() > 200 && colour.red() < 150; }

TEST_F(ImageViewTest, OverlaysFollowZoomAndPan) {
    // Image pixels 40 to 60 are view pixels 80 to 120 at the fitted scale of 2.
    const auto box = viewer_core::makeOverlays({viewer_core::Rectangle{{40, 40, 20, 20}}});
    m_view.setImage(blackImage(100, 100), ImageView::KeepView::No, box);
    ASSERT_TRUE(isEdge(pixelOf(m_view, 80, 100)));

    sendWheelEvent(m_view, {100, 100}, 120); // keeps view point (100, 100), image point (50, 50), in place
    const double scale =
        m_view.deviceScaleFor(QSize(100, 100), ImageView::KeepView::IfSameSize) / m_view.devicePixelRatioF();
    const int left = static_cast<int>(std::lround(100 - 10 * scale)); // image x 40 is 10 left of the centre

    ASSERT_GT(scale, 2.0);
    bool found = false;
    for (int x = left - 2; x <= left + 2; ++x) {
        found = found || isEdge(pixelOf(m_view, x, 100));
    }
    EXPECT_TRUE(found) << "expected the edge near x = " << left;
}

TEST_F(ImageViewTest, OverlaysOnASmallerPicture) {
    m_view.setImage(blackImage(50, 50), ImageView::KeepView::No, QSize(100, 100), leftHalf());

    EXPECT_FALSE(isBlack(pixelOf(m_view, 50, 100)));
    EXPECT_TRUE(isBlack(pixelOf(m_view, 150, 100)));
}

TEST_F(ImageViewTest, ANewPictureWithoutOverlaysShowsNone) {
    m_view.setImage(blackImage(100, 100), ImageView::KeepView::No, leftHalf());

    m_view.setImage(blackImage(100, 100), ImageView::KeepView::IfSameSize);

    EXPECT_TRUE(m_view.overlays().empty());
    EXPECT_TRUE(isBlack(pixelOf(m_view, 50, 100)));
}

TEST_F(ImageViewTest, HiddenOverlaysDrawNothing) {
    m_view.setImage(blackImage(100, 100), ImageView::KeepView::No, leftHalf());
    EXPECT_TRUE(m_view.overlaysShown());

    m_view.setOverlaysShown(false);

    EXPECT_FALSE(m_view.overlaysShown());
    EXPECT_EQ(m_view.overlays().size(), 1U); // kept, only not drawn
    EXPECT_TRUE(isBlack(pixelOf(m_view, 50, 100)));
    m_view.setOverlaysShown(true);
    EXPECT_FALSE(isBlack(pixelOf(m_view, 50, 100)));
}

TEST_F(ImageViewTest, ClearForgetsOverlays) {
    m_view.setImage(blackImage(100, 100), ImageView::KeepView::No, leftHalf());

    m_view.clear();

    EXPECT_TRUE(m_view.overlays().empty());
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OVERLAY_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define OVERLAY_TEST_SANITIZED 1
#endif

// Zoomed in as far as the view goes, with shapes reaching a million pixels away: a polygon and a rectangle
// that cover the view, and 254 small rectangles out of it. Painting must stay bounded and show the fill.
TEST_F(ImageViewTest, FarAwayAndHugeOverlaysPaint) {
    std::vector<viewer_core::Overlay> overlays;
    std::vector<cv::Point2d> ring;
    for (int i = 0; i < 1024; ++i) {
        const double angle = 2 * 3.14159265358979 * i / 1024;
        ring.emplace_back(50 + 999000 * std::cos(angle), 50 + 999000 * std::sin(angle));
    }
    overlays.push_back(viewer_core::Polygon{ring, "ring"});
    overlays.push_back(viewer_core::Rectangle{{-1e6, -1e6, 2e6, 2e6}, "huge"});
    for (int i = 0; i < 254; ++i) {
        overlays.push_back(viewer_core::Rectangle{{900000.0 + i, -900000.0, 5, 5}, "far"});
    }
    m_view.setImage(blackImage(100, 100), ImageView::KeepView::No, viewer_core::makeOverlays(overlays));
    for (int notch = 0; notch < 40; ++notch) {
        sendWheelEvent(m_view, {100, 100}, 120);
    }
    // The first text a process lays out loads the fonts: on a Windows runner the first such test of a run
    // reads all of %WINDIR%\Fonts from a cold disk, seconds that are not painting. So the fonts load before
    // the clock starts; the labels themselves are still laid out in the timed paint.
    QFontMetrics(m_view.font()).horizontalAdvance(QStringLiteral("far"));

    QElapsedTimer timer;
    timer.start();
    const QColor centre = pixelOf(m_view, 100, 100);
    const qint64 elapsed = timer.elapsed();

    std::cout << "[ overlays ] zoomed-in paint with far and huge overlays: " << elapsed << " ms\n";
    EXPECT_FALSE(isBlack(centre));
#if !defined(OVERLAY_TEST_SANITIZED)
    EXPECT_LT(elapsed, 1000);
#endif
}

// The time a paint takes, as a 4K live frame drawn at 1600×900 is: with no overlays, then with the heaviest
// list the wire takes in practice (256 overlays, one a 1024-point polygon, the rest labelled rectangles).
// Run by hand in a release build: --gtest_also_run_disabled_tests --gtest_filter=*OverlayPaintCost*
TEST(ImageViewMeasurement, DISABLED_OverlayPaintCost) {
    ImageView view;
    view.resize(1600, 900);
    view.show();
    std::vector<viewer_core::Overlay> overlays;
    std::vector<cv::Point2d> ring;
    for (int i = 0; i < 1024; ++i) {
        const double angle = 2 * 3.14159265358979 * i / 1024;
        ring.emplace_back(1920 + 900 * std::cos(angle), 1080 + 900 * std::sin(angle));
    }
    overlays.push_back(viewer_core::Polygon{ring, "Document"});
    for (int i = 0; i < 255; ++i) {
        overlays.push_back(viewer_core::Rectangle{{100.0 + (i % 17) * 210, 100.0 + (i / 17) * 130, 180, 100},
                                                  std::string(64, 'l')});
    }
    const viewer_core::ImageTags tags{.overlays = viewer_core::makeOverlays(overlays)};
    const auto encoded =
        visual_debug::encode(cv::Mat(2160, 3840, CV_8UC3), "frame", {.description = toDescription(tags)});
    ASSERT_TRUE(std::holds_alternative<visual_debug::EncodedMessage>(encoded)) << "the list must fit 64 KiB";
    std::cout << "[ overlays ] header bytes: "
              << std::get<visual_debug::EncodedMessage>(encoded).header.size() << '\n';
    QImage picture(1600, 900, QImage::Format_RGBA8888);
    picture.fill(Qt::darkGray);

    // render() calls paintEvent into an image made once, so only the paint is timed (an offscreen window
    // is never exposed, so repaint() would paint nothing).
    QImage target(view.size(), QImage::Format_ARGB32_Premultiplied);
    // A Retina screen's backing store: twice the pixels, with the painter scaled by 2. Labels laid out once
    // must stay laid out there too.
    QImage retina(view.size() * 2, QImage::Format_ARGB32_Premultiplied);
    retina.setDevicePixelRatio(2.0);
    const auto meanPaintOn = [&](QImage& onto, viewer_core::ImageTags::Overlays list) {
        view.setImage(picture, ImageView::KeepView::No, QSize(3840, 2160), list);
        view.render(&onto); // the first paint loads fonts
        constexpr int kPaints = 100;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < kPaints; ++i) {
            view.render(&onto);
        }
        return static_cast<double>(timer.nsecsElapsed()) / kPaints / 1e6;
    };
    const auto meanPaint = [&](viewer_core::ImageTags::Overlays list) { return meanPaintOn(target, list); };
    const double without = meanPaint(nullptr);
    const double with = meanPaint(tags.overlays);
    // The parts of the cost: the polygon alone, the rectangles without and with their labels.
    std::vector<viewer_core::Overlay> unlabelled(overlays.begin() + 1, overlays.end());
    for (auto& overlay : unlabelled) {
        std::get<viewer_core::Rectangle>(overlay).label.clear();
    }
    const double polygonOnly = meanPaint(viewer_core::makeOverlays({overlays.front()}));
    const double rectangles = meanPaint(viewer_core::makeOverlays(unlabelled));
    const double labelled = meanPaint(viewer_core::makeOverlays({overlays.begin() + 1, overlays.end()}));
    // What a document check sends: the outline, the photo, the MRZ band, the face and four corner points.
    const double typical = meanPaint(viewer_core::makeOverlays(
        {viewer_core::Polygon{{{900, 500}, {2900, 450}, {2950, 1700}, {870, 1750}}, "Document"},
         viewer_core::Rectangle{{1050, 700, 500, 640}, "Photo"},
         viewer_core::Polygon{
             {{950, 1450}, {2850, 1420}, {2855, 1620}, {955, 1650}}, "MRZ", {0xE0, 0x50, 0x50}},
         viewer_core::Circle{{1300, 1000}, 200}, viewer_core::Point{{900, 500}},
         viewer_core::Point{{2900, 450}}, viewer_core::Point{{2950, 1700}},
         viewer_core::Point{{870, 1750}}}));

    std::cout << "[ overlays ] mean paint without: " << without << " ms, with 256 overlays: " << with
              << " ms, added: " << with - without << " ms (bar 1.67 ms)\n"
              << "[ overlays ] the 1024-point polygon alone: " << polygonOnly
              << " ms; 255 rectangles: " << rectangles << " ms; with their labels: " << labelled << " ms\n"
              << "[ overlays ] a document check's 8 overlays: " << typical << " ms\n";
    // At a device pixel ratio of 2: the rectangles without and with their labels. Labels laid out again on
    // every paint would make the second far more than the first, as they did before the label cache (about 10
    // ms).
    const double retinaRectangles = meanPaintOn(retina, viewer_core::makeOverlays(unlabelled));
    const double retinaLabelled =
        meanPaintOn(retina, viewer_core::makeOverlays({overlays.begin() + 1, overlays.end()}));
    std::cout << "[ overlays ] at DPR 2: 255 rectangles: " << retinaRectangles
              << " ms; with their labels: " << retinaLabelled << " ms\n";
}

} // namespace
