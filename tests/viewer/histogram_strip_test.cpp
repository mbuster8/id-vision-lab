#include "histogram_strip.h"

#include "viewer_core/histogram.h"

#include <QAction>
#include <QColor>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSettings>
#include <QString>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

namespace {

using viewer_core::DisplayRange;
using viewer_core::Histogram;
using viewer_core::RangeMarkers;

// What the window would hand the strip for an image and its markers.
struct Shown {
    Histogram histogram;
    std::optional<RangeMarkers> markers;
};

Shown shownFor(const cv::Mat& image, const std::optional<RangeMarkers>& markers,
               std::size_t cap = viewer_core::kHistogramSamples) {
    return {viewer_core::histogramOf(image, viewer_core::valueExtent(image), markers, cap), markers};
}

Shown autoShown(const cv::Mat& image, std::size_t cap = viewer_core::kHistogramSamples) {
    const viewer_core::ValueExtent extent = viewer_core::valueExtent(image);
    const auto markers = viewer_core::rangeMarkers(image.depth(), image.channels(), {}, {}, extent);
    return {viewer_core::histogramOf(image, extent, markers, cap), markers};
}

// 0 and 100 in floating point, with a range of 20 to 80 unless told otherwise.
Shown percent(const RangeMarkers& markers = {.range = {20, 80}}) {
    cv::Mat image(1, 2, CV_32FC1);
    image.at<float>(0, 0) = 0.0F;
    image.at<float>(0, 1) = 100.0F;
    return shownFor(image, markers);
}

void sendMouse(QWidget& widget, QEvent::Type type, double x, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    const QPointF position(x, widget.height() / 2.0);
    QMouseEvent event(type, position, widget.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

void press(QWidget& widget, double x) {
    sendMouse(widget, QEvent::MouseButtonPress, x, Qt::LeftButton, Qt::LeftButton);
}
void move(QWidget& widget, double x) {
    sendMouse(widget, QEvent::MouseMove, x, Qt::NoButton, Qt::LeftButton);
}
void release(QWidget& widget, double x) {
    sendMouse(widget, QEvent::MouseButtonRelease, x, Qt::LeftButton, Qt::NoButton);
}

// As a platform delivers one: the first click, then the second as a press and a double-click.
void doubleClick(QWidget& widget, double x) {
    press(widget, x);
    release(widget, x);
    press(widget, x);
    sendMouse(widget, QEvent::MouseButtonDblClick, x, Qt::LeftButton, Qt::LeftButton);
    release(widget, x);
}

// A shown strip, 400 wide, that keeps what it emits.
class HistogramStripTest : public testing::Test {
protected:
    void SetUp() override {
        m_strip.resize(400, HistogramStrip::kHeight);
        m_strip.show();
        QObject::connect(&m_strip, &HistogramStrip::rangeDragged, [this](DisplayRange range, bool hdr) {
            m_ranges.push_back(range);
            m_hdr.push_back(hdr);
        });
        QObject::connect(&m_strip, &HistogramStrip::resetRequested, [this] { ++m_resets; });
    }

    void show(const Shown& shown) { m_strip.setHistogram(shown.histogram, shown.markers); }

    // The values one pixel of the plot spans.
    double onePixel() const { return 1.0 / (m_strip.xOf(1.0) - m_strip.xOf(0.0)); }

    void expectLastRangeNear(const DisplayRange& expected) {
        ASSERT_FALSE(m_ranges.empty());
        EXPECT_NEAR(m_ranges.back().min, expected.min, onePixel());
        EXPECT_NEAR(m_ranges.back().max, expected.max, onePixel());
    }

    HistogramStrip m_strip;
    std::vector<DisplayRange> m_ranges;
    std::vector<bool> m_hdr;
    int m_resets = 0;
};

TEST(HistogramStrip, StartsHiddenEmptyAndShort) {
    const HistogramStrip strip;

    EXPECT_FALSE(strip.isVisible());
    EXPECT_EQ(strip.sizeHint().height(), HistogramStrip::kHeight);
    EXPECT_TRUE(strip.histogram().counts.empty());
    EXPECT_FALSE(strip.markers());
}

TEST_F(HistogramStripTest, DraggingAnEndEmitsTheNewRange) {
    show(percent());

    press(m_strip, m_strip.xOf(20));
    move(m_strip, m_strip.xOf(40));
    QCoreApplication::processEvents();
    release(m_strip, m_strip.xOf(40));

    expectLastRangeNear({40, 80});
    ASSERT_FALSE(m_hdr.empty());
    EXPECT_FALSE(m_hdr.back());
    EXPECT_FALSE(m_strip.isDragging());
}

TEST_F(HistogramStripTest, MarkersFollowTheDrag) {
    show(percent());
    const DisplayRange axis = m_strip.histogram().axis;

    press(m_strip, m_strip.xOf(20));
    move(m_strip, m_strip.xOf(40));

    ASSERT_TRUE(m_strip.markers());
    EXPECT_NEAR(m_strip.markers()->range.min, 40.0, onePixel());
    EXPECT_EQ(m_strip.histogram().axis, axis);
    release(m_strip, m_strip.xOf(40));
}

TEST_F(HistogramStripTest, DraggingTheMiddleKeepsTheWidth) {
    show(percent());

    press(m_strip, m_strip.xOf(50));
    move(m_strip, m_strip.xOf(60));
    QCoreApplication::processEvents();
    release(m_strip, m_strip.xOf(60));

    expectLastRangeNear({30, 90});
}

TEST_F(HistogramStripTest, PressAwayFromTheMarkersDoesNothing) {
    show(percent());

    press(m_strip, m_strip.xOf(5));
    move(m_strip, m_strip.xOf(10));
    QCoreApplication::processEvents();
    release(m_strip, m_strip.xOf(10));

    EXPECT_TRUE(m_ranges.empty());
}

TEST_F(HistogramStripTest, NoMarkersNoDrag) {
    show({percent().histogram, std::nullopt});

    for (const double value : {0.0, 20.0, 50.0, 80.0}) {
        press(m_strip, m_strip.xOf(value));
        move(m_strip, m_strip.xOf(value + 5));
        QCoreApplication::processEvents();
        release(m_strip, m_strip.xOf(value + 5));
    }

    EXPECT_TRUE(m_ranges.empty());
}

TEST_F(HistogramStripTest, DragsAreCoalesced) {
    show(percent());

    press(m_strip, m_strip.xOf(80));
    for (int step = 1; step <= 10; ++step) {
        move(m_strip, m_strip.xOf(80 + step));
    }
    EXPECT_TRUE(m_ranges.empty()); // nothing until the event loop turns
    QCoreApplication::processEvents();

    ASSERT_EQ(m_ranges.size(), 1u);
    expectLastRangeNear({20, 90});
    release(m_strip, m_strip.xOf(90));
    EXPECT_EQ(m_ranges.size(), 1u); // nothing was pending
}

TEST_F(HistogramStripTest, HdrMarkersDragAsHdr) {
    show(percent({.range = {20, 80}, .scaleTop = 100, .hdr = true}));

    press(m_strip, m_strip.xOf(80));
    move(m_strip, m_strip.xOf(70));
    release(m_strip, m_strip.xOf(70));

    ASSERT_EQ(m_hdr.size(), 1u);
    EXPECT_TRUE(m_hdr.back());
}

TEST_F(HistogramStripTest, AxisAndMarkersHoldDuringADrag) {
    show(percent());
    const double at20 = m_strip.xOf(20);
    cv::Mat other(1, 2, CV_32FC1);
    other.at<float>(0, 0) = 0.0F;
    other.at<float>(0, 1) = 1000.0F;
    const Shown next = shownFor(other, RangeMarkers{.range = {100, 900}, .scaleTop = 900, .hdr = true});

    press(m_strip, at20);
    show(next);
    EXPECT_EQ(m_strip.xOf(20), at20);
    ASSERT_TRUE(m_strip.markers());
    EXPECT_FALSE(m_strip.markers()->hdr);
    move(m_strip, m_strip.xOf(30));
    QCoreApplication::processEvents();
    ASSERT_FALSE(m_hdr.empty());
    EXPECT_FALSE(m_hdr.back());
    release(m_strip, m_strip.xOf(30));

    EXPECT_EQ(m_strip.histogram().axis, next.histogram.axis);
    EXPECT_EQ(m_strip.markers(), next.markers);
}

TEST_F(HistogramStripTest, ReleaseAppliesAHistogramSetDuringIt) {
    show(percent());
    cv::Mat other(1, 2, CV_32FC1);
    other.at<float>(0, 0) = 0.0F;
    other.at<float>(0, 1) = 1000.0F;
    const Shown next = shownFor(other, RangeMarkers{.range = {300, 700}});
    // As the window does: the drag's range redraws the picture, which hands the strip a new histogram.
    const QMetaObject::Connection redraw =
        QObject::connect(&m_strip, &HistogramStrip::rangeDragged, [this, &next] { show(next); });

    press(m_strip, m_strip.xOf(20));
    move(m_strip, m_strip.xOf(30));
    release(m_strip, m_strip.xOf(30)); // emits the pending range, while still dragging

    QObject::disconnect(redraw);
    ASSERT_EQ(m_ranges.size(), 1u);
    EXPECT_EQ(m_strip.histogram().axis, next.histogram.axis);
    EXPECT_EQ(m_strip.markers(), next.markers);
}

TEST_F(HistogramStripTest, ClearAndHideCancelTheDrag) {
    const Shown shown = percent();
    show(shown);
    press(m_strip, m_strip.xOf(20));
    move(m_strip, m_strip.xOf(30));
    m_strip.clear();
    QCoreApplication::processEvents();

    EXPECT_TRUE(m_ranges.empty());
    EXPECT_FALSE(m_strip.isDragging());
    show(shown);
    EXPECT_EQ(m_strip.histogram().axis, shown.histogram.axis); // applied at once

    press(m_strip, m_strip.xOf(20));
    move(m_strip, m_strip.xOf(30));
    m_strip.hide();
    QCoreApplication::processEvents();

    EXPECT_TRUE(m_ranges.empty());
    EXPECT_FALSE(m_strip.isDragging());
}

// A click whose hand trembles, a pixel up or a few across, is not a drag: it must not fix the range.
TEST_F(HistogramStripTest, AJitterIsNotADrag) {
    show(percent());
    const double middle = m_strip.xOf(50);

    press(m_strip, middle);
    const QPointF below(middle, m_strip.height() / 2.0 + 1);
    QMouseEvent wobble(QEvent::MouseMove, below, m_strip.mapToGlobal(below), Qt::NoButton, Qt::LeftButton,
                       Qt::NoModifier);
    QCoreApplication::sendEvent(&m_strip, &wobble);
    move(m_strip, middle + 2);
    QCoreApplication::processEvents();
    release(m_strip, middle + 2);
    QCoreApplication::processEvents();

    EXPECT_TRUE(m_ranges.empty());
}

TEST_F(HistogramStripTest, RightClickEndsADrag) {
    show(percent());
    press(m_strip, m_strip.xOf(20));
    move(m_strip, m_strip.xOf(40)); // pending

    const QPoint centre = m_strip.rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, centre, m_strip.mapToGlobal(centre));
    QCoreApplication::sendEvent(&m_strip, &event);
    QCoreApplication::processEvents();
    m_strip.findChild<QMenu*>(QStringLiteral("histogramMenu"))->hide();

    EXPECT_TRUE(m_ranges.empty());
    EXPECT_FALSE(m_strip.isDragging()); // the menu takes the mouse, so no release would come
}

// Whether anything was painted in a rectangle of an image over a background of one colour.
bool paintedIn(const QImage& image, const QRect& area, QColor background) {
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (image.pixelColor(x, y) != background) {
                return true;
            }
        }
    }
    return false;
}

// The strip's own painting only: without RenderFlags, render() would first fill the widget's background, and
// every pixel would differ from the one filled here.
QImage rendered(HistogramStrip& strip, QColor background) {
    QImage target(strip.size(), QImage::Format_ARGB32_Premultiplied);
    target.fill(background);
    strip.render(&target, QPoint(), QRegion(), QWidget::RenderFlags());
    return target;
}

TEST_F(HistogramStripTest, AxisEndsAreLabelled) {
    show(percent({.range = {40, 60}})); // the markers' own labels far from either end
    const QColor background(1, 2, 3);

    const QImage image = rendered(m_strip, background);

    // The row of text under the plot, where the curves never reach.
    const int textHeight = m_strip.fontMetrics().height();
    const QRect leftEnd(0, m_strip.height() - textHeight, 12, textHeight);
    const QRect rightEnd(m_strip.width() - 12, m_strip.height() - textHeight, 12, textHeight);
    EXPECT_TRUE(paintedIn(image, leftEnd, background));
    EXPECT_TRUE(paintedIn(image, rightEnd, background));
}

TEST_F(HistogramStripTest, SaysWhenNothingIsFinite) {
    const QColor background(1, 2, 3);
    show(autoShown(cv::Mat(4, 4, CV_32FC1, cv::Scalar(std::nan(""))))); // Auto: no markers either

    EXPECT_TRUE(paintedIn(rendered(m_strip, background), m_strip.rect(), background));
}

TEST_F(HistogramStripTest, DoubleClickRequestsResetOnly) {
    show(percent());

    doubleClick(m_strip, m_strip.xOf(50));
    QCoreApplication::processEvents();

    EXPECT_EQ(m_resets, 1);
    EXPECT_TRUE(m_ranges.empty());
    EXPECT_FALSE(m_strip.isDragging());
}

TEST_F(HistogramStripTest, RightClickOffersLinearAndLog) {
    show(percent());
    const QPoint centre = m_strip.rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, centre, m_strip.mapToGlobal(centre));
    QCoreApplication::sendEvent(&m_strip, &event);

    auto* menu = m_strip.findChild<QMenu*>(QStringLiteral("histogramMenu"));
    ASSERT_NE(menu, nullptr);
    EXPECT_TRUE(menu->isVisible());
    EXPECT_FALSE(m_strip.logCounts());
    m_strip.findChild<QAction*>(QStringLiteral("histogramLog"))->trigger();
    menu->hide();

    EXPECT_TRUE(m_strip.logCounts());
    EXPECT_TRUE(QSettings().value(QStringLiteral("viewer/histogramLog")).toBool());
    EXPECT_TRUE(HistogramStrip().logCounts());
}

TEST_F(HistogramStripTest, BinTextNamesTheSpanAndShares) {
    cv::Mat image(2, 2, CV_8UC3, cv::Scalar::all(10));
    image.row(1).setTo(cv::Scalar::all(200));
    show(autoShown(image));

    const QString text = m_strip.binText(qRound(m_strip.xOf(10.5)));

    EXPECT_TRUE(text.startsWith(QStringLiteral("10"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("B 50.0 %"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("sampled"))) << text.toStdString();
}

TEST_F(HistogramStripTest, BinTextSaysWhenSampled) {
    show(autoShown(cv::Mat(100, 100, CV_8UC1, cv::Scalar(5)), 1000));

    const QString text = m_strip.binText(qRound(m_strip.xOf(5.5)));

    EXPECT_TRUE(text.endsWith(QStringLiteral("sampled: 1 pixel in 16"))) << text.toStdString();
}

// Whether anything was painted over a background of one colour.
bool paintedSomething(const QImage& image, QColor background) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) != background) {
                return true;
            }
        }
    }
    return false;
}

TEST_F(HistogramStripTest, RendersEveryKindAtAnyWidth) {
    const QColor background(1, 2, 3);
    std::vector<Shown> kinds;
    for (const int type : {CV_8UC1, CV_8UC2, CV_8UC3, CV_8UC4}) {
        cv::Mat image(8, 8, type);
        cv::randu(image, cv::Scalar::all(0), cv::Scalar::all(256));
        kinds.push_back(autoShown(image));
    }
    kinds.push_back(shownFor(cv::Mat(4, 4, CV_16UC3, cv::Scalar::all(30000)),
                             RangeMarkers{.range = {4100, 60218}, .scaleTop = 65535, .hdr = true}));
    kinds.push_back(shownFor(cv::Mat(4, 4, CV_32FC1, cv::Scalar(std::nan(""))), std::nullopt));
    m_strip.findChild<QAction*>(QStringLiteral("histogramLog"))->trigger();

    for (const int width : {400, 50}) {
        m_strip.resize(width, HistogramStrip::kHeight);
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            show(kinds[i]);
            QImage target(m_strip.size(), QImage::Format_ARGB32_Premultiplied);
            target.fill(background);
            m_strip.render(&target, QPoint(), QRegion(), QWidget::RenderFlags()); // its painting alone
            if (width == 400) {
                EXPECT_TRUE(paintedSomething(target, background)) << i; // nothing finite says so
            }
        }
    }
}

// What a paint of the strip costs, as under a 1600-pixel-wide view showing a 4K 16UC3 frame, at 1× and on a
// Retina screen's backing store. render() paints into an image made once, so only the paint is timed (an
// offscreen window is never exposed, so repaint() would paint nothing). Run by hand in a release build:
// --gtest_also_run_disabled_tests --gtest_filter=*HistogramStripMeasurement*
TEST(HistogramStripMeasurement, DISABLED_PaintCost) {
    HistogramStrip strip;
    strip.resize(1600, HistogramStrip::kHeight);
    strip.show();
    cv::Mat frame(2160, 3840, CV_16UC3);
    cv::randu(frame, cv::Scalar::all(0), cv::Scalar::all(65536));
    const Shown shown = autoShown(frame);
    strip.setHistogram(shown.histogram, shown.markers);

    for (const double ratio : {1.0, 2.0}) {
        QImage target(strip.size() * ratio, QImage::Format_ARGB32_Premultiplied);
        target.setDevicePixelRatio(ratio);
        std::vector<double> times;
        for (int run = 0; run < 41; ++run) {
            QElapsedTimer timer;
            timer.start();
            strip.render(&target);
            times.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
        }
        std::sort(times.begin() + 1, times.end()); // the first warms up
        std::cout << "[ histogram strip ] paint at " << ratio << "x: " << times[times.size() / 2] << " ms\n";
    }
}

} // namespace
