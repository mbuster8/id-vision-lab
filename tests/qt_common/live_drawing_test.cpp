#include "image_view.h"
#include "live_drawing.h"

#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include <optional>

namespace {

const cv::Size kFrame{3840, 2160};
const QSize kFrameSize(kFrame.width, kFrame.height);

QImage filledImage(int width, int height) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::gray);
    return image;
}

// One notch of a mouse wheel in, at the view's centre.
void zoomIn(ImageView& view) {
    const QPointF centre = QRectF(view.rect()).center();
    QWheelEvent event(centre, view.mapToGlobal(centre), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&view, &event);
}

void sendMouse(ImageView& view, QEvent::Type type, QPointF position) {
    QMouseEvent event(type, position, view.mapToGlobal(position), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &event);
}

void pan(ImageView& view, QPointF from, QPointF to) {
    sendMouse(view, QEvent::MouseButtonPress, from);
    sendMouse(view, QEvent::MouseMove, to);
    sendMouse(view, QEvent::MouseButtonRelease, to);
}

// An 800×600 view: a 3840×2160 frame fits at 800×450.
class LiveDrawingTest : public testing::Test {
protected:
    void SetUp() override {
        m_view.resize(800, 600);
        m_view.show();
        m_view.setImage(filledImage(kFrame.width, kFrame.height));
    }

    // Zoomed in until less than half the frame's width is in view, so the cut with its margins is well under
    // three quarters of the frame.
    void zoomToLessThanHalf() {
        for (int i = 0; i < 30 && m_view.visibleArea().width() >= kFrame.width / 2; ++i) {
            zoomIn(m_view);
        }
        ASSERT_LT(m_view.visibleArea().width(), kFrame.width / 2);
    }

    ImageView m_view;
};

} // namespace

TEST(DrawnSizeOf, KeepsThePartAtOneOrMore) {
    EXPECT_EQ(drawnSizeOf({1000, 500}, 1.0), cv::Size(1000, 500));
    EXPECT_EQ(drawnSizeOf({1000, 500}, 2.0), cv::Size(1000, 500));
}

TEST(DrawnSizeOf, ShrinksBelowOne) { EXPECT_EQ(drawnSizeOf({1000, 500}, 0.5), cv::Size(500, 250)); }

TEST_F(LiveDrawingTest, FittedIsTheWholeFrame) {
    const LiveDrawing drawing = liveDrawing(m_view, kFrame, ImageView::KeepView::IfSameSize);

    EXPECT_EQ(drawing.area, cv::Rect(0, 0, kFrame.width, kFrame.height));
    EXPECT_EQ(drawing.size,
              drawnSizeOf(kFrame, m_view.deviceScaleFor(kFrameSize, ImageView::KeepView::IfSameSize)));
    EXPECT_EQ(drawing.size, cv::Size(800, 450)); // the offscreen platform's device pixel ratio is 1
}

TEST_F(LiveDrawingTest, ZoomedInCutsTheViewAndAMargin) {
    zoomToLessThanHalf();
    const QRect visible = m_view.visibleArea();

    const LiveDrawing drawing = liveDrawing(m_view, kFrame, ImageView::KeepView::IfSameSize);

    const int marginX = visible.width() / 4;
    const int marginY = visible.height() / 4;
    const cv::Rect whole(0, 0, kFrame.width, kFrame.height);
    const cv::Rect expected = cv::Rect(visible.x() - marginX, visible.y() - marginY,
                                       visible.width() + 2 * marginX, visible.height() + 2 * marginY) &
                              whole;
    EXPECT_EQ(drawing.area, expected);
    EXPECT_LT(static_cast<double>(drawing.area.area()), 0.75 * static_cast<double>(whole.area()));
}

TEST_F(LiveDrawingTest, AFitAboutToComeIsTheWholeFrame) {
    zoomToLessThanHalf();

    const LiveDrawing drawing = liveDrawing(m_view, kFrame, ImageView::KeepView::No);

    EXPECT_EQ(drawing.area, cv::Rect(0, 0, kFrame.width, kFrame.height));
}

TEST_F(LiveDrawingTest, NoRedrawWhenTheNewestCoversTheView) {
    zoomToLessThanHalf();
    const LiveDrawing newest = liveDrawing(m_view, kFrame, ImageView::KeepView::IfSameSize);

    EXPECT_FALSE(needsRedraw(m_view, kFrame, newest));
}

TEST_F(LiveDrawingTest, NoRedrawForAPanWithinTheMargin) {
    zoomToLessThanHalf();
    const LiveDrawing newest = liveDrawing(m_view, kFrame, ImageView::KeepView::IfSameSize);
    const QPointF centre = QRectF(m_view.rect()).center();

    pan(m_view, centre, centre - QPointF(10, 0));

    EXPECT_FALSE(needsRedraw(m_view, kFrame, newest));
}

TEST_F(LiveDrawingTest, RedrawAfterPanningOutOfTheMargin) {
    zoomToLessThanHalf();
    const LiveDrawing newest = liveDrawing(m_view, kFrame, ImageView::KeepView::IfSameSize);
    const QPointF centre = QRectF(m_view.rect()).center();

    pan(m_view, centre, centre - QPointF(m_view.width() / 2.0, 0));

    EXPECT_TRUE(needsRedraw(m_view, kFrame, newest));
}

TEST_F(LiveDrawingTest, RedrawAfterZoomingFurtherBelowOneToOne) {
    zoomToLessThanHalf();
    const LiveDrawing newest = liveDrawing(m_view, kFrame, ImageView::KeepView::IfSameSize);
    zoomIn(m_view);
    // Still below 1:1, so the area is drawn larger than it was converted.
    ASSERT_LT(m_view.deviceScaleFor(kFrameSize, ImageView::KeepView::IfSameSize), 1.0);

    EXPECT_TRUE(needsRedraw(m_view, kFrame, newest));
}

TEST_F(LiveDrawingTest, RedrawWithoutANewest) { EXPECT_TRUE(needsRedraw(m_view, kFrame, std::nullopt)); }
