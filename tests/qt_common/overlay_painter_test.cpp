#include "overlay_painter.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QTransform>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using viewer_core::Circle;
using viewer_core::Colour;
using viewer_core::kOverlayGreen;
using viewer_core::Overlay;
using viewer_core::Point;
using viewer_core::Polygon;
using viewer_core::Rectangle;

constexpr Colour kRed{0xE0, 0x50, 0x50};

QImage paintOn(const std::vector<Overlay>& overlays, double scale = 1.0, QColor background = Qt::black) {
    QImage image(200, 200, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    qt_common::paintOverlays(painter, overlays, QTransform::fromScale(scale, scale));
    painter.end();
    return image;
}

bool near(const QColor& actual, int r, int g, int b, int tolerance = 3) {
    return std::abs(actual.red() - r) <= tolerance && std::abs(actual.green() - g) <= tolerance &&
           std::abs(actual.blue() - b) <= tolerance;
}

bool isFull(const QImage& image, int x, int y, Colour colour = kOverlayGreen) {
    return near(image.pixelColor(x, y), colour.r, colour.g, colour.b);
}

// Consecutive full-colour pixels from (x, y), stepping by (dx, dy).
int runFrom(const QImage& image, int x, int y, int dx, int dy) {
    int run = 0;
    while (x >= 0 && y >= 0 && x < image.width() && y < image.height() && isFull(image, x, y)) {
        ++run;
        x += dx;
        y += dy;
    }
    return run;
}

// The length of the first run of full-colour pixels along a row, scanning from x = from.
int firstRunInRow(const QImage& image, int y, int from) {
    int x = from;
    while (x < image.width() && !isFull(image, x, y)) {
        ++x;
    }
    return runFrom(image, x, y, 1, 0);
}

TEST(OverlayPainter, FillIsSeeThrough) {
    const QImage image = paintOn({Rectangle{{50, 50, 100, 100}}});

    EXPECT_TRUE(near(image.pixelColor(100, 100), 30, 58, 30))
        << image.pixelColor(100, 100).name().toStdString();
}

TEST(OverlayPainter, EdgeIsSolidAndTwoPixelsWide) {
    const QImage plain = paintOn({Rectangle{{50, 50, 100, 100}}});
    const QImage zoomed = paintOn({Rectangle{{10, 10, 30, 30}}}, 4.0); // edges at 40 and 160

    EXPECT_EQ(firstRunInRow(plain, 100, 40), 2);
    EXPECT_EQ(firstRunInRow(zoomed, 100, 30), 2);
}

TEST(OverlayPainter, PolygonIsClosed) {
    const QImage image = paintOn({Polygon{{{40, 40}, {160, 40}, {100, 160}}}});

    // Around the midpoint of the side from the last point back to the first: a 2 px line at this slope
    // covers at least one pixel of the row fully.
    bool edge = false;
    for (int x = 67; x <= 73; ++x) {
        edge = edge || isFull(image, x, 100);
    }
    EXPECT_TRUE(edge);
}

// Points on one line, level or upright, have a bounding box of zero height or width: still drawn, as a line.
TEST(OverlayPainter, PolygonOnOneLineIsDrawn) {
    const QImage level = paintOn({Polygon{{{40, 100}, {100, 100}, {160, 100}}}});
    const QImage upright = paintOn({Polygon{{{100, 40}, {100, 100}, {100, 160}}}});

    EXPECT_TRUE(isFull(level, 100, 100) || isFull(level, 100, 99));
    EXPECT_TRUE(isFull(upright, 100, 100) || isFull(upright, 99, 100));
}

TEST(OverlayPainter, CircleHasAnEdge) {
    const QImage image = paintOn({Circle{{100, 100}, 50}});

    EXPECT_TRUE(isFull(image, 150, 100) || isFull(image, 149, 100));
    EXPECT_TRUE(near(image.pixelColor(100, 100), 30, 58, 30));
}

// Upward from the shape's top edge, through the tab above it: the edge's outer pixel plus the tab's height.
int tabHeight(const QImage& image, int left, int top) { return runFrom(image, left + 1, top, 0, -1); }

TEST(OverlayPainter, LabelTabKeepsItsSizeWhenZoomed) {
    const QImage plain = paintOn({Rectangle{{20, 40, 20, 20}, "MRZ"}});
    const QImage zoomed = paintOn({Rectangle{{20, 40, 20, 20}, "MRZ"}}, 3.0); // at 60, 120

    const int plainHeight = tabHeight(plain, 20, 40);
    EXPECT_GT(plainHeight, 8);
    EXPECT_NEAR(tabHeight(zoomed, 60, 120), plainHeight, 1);
    const int plainWidth = runFrom(plain, 19, 37, 1, 0);
    EXPECT_GT(plainWidth, 15);
    EXPECT_NEAR(runFrom(zoomed, 59, 117, 1, 0), plainWidth, 1);
}

TEST(OverlayPainter, NoLabelNoTab) {
    const QImage image = paintOn({Rectangle{{20, 40, 20, 20}}});

    EXPECT_LE(tabHeight(image, 20, 40), 2);
}

double relativeLuminance(const QColor& colour) {
    const auto linear = [](double c) {
        c /= 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(colour.red()) + 0.7152 * linear(colour.green()) + 0.0722 * linear(colour.blue());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

TEST(OverlayPainter, LabelTextIsDarkOnLightAndLightOnDark) {
    EXPECT_LT(qt_common::labelTextColour(kOverlayGreen).lightness(), 128);
    EXPECT_GT(qt_common::labelTextColour({0x20, 0x20, 0x80}).lightness(), 128);
    // The one of black and white that contrasts more with a mid red.
    const QColor red(kRed.r, kRed.g, kRed.b);
    const QColor better =
        contrast(red, Qt::black) > contrast(red, Qt::white) ? QColor(Qt::black) : QColor(Qt::white);
    EXPECT_EQ(qt_common::labelTextColour(kRed).lightness() < 128, better.lightness() < 128);
}

int differingRun(const QImage& image, int y, int from, const QColor& background) {
    int run = 0;
    for (int x = from; x < image.width(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        if (std::abs(pixel.red() - background.red()) + std::abs(pixel.green() - background.green()) +
                std::abs(pixel.blue() - background.blue()) >
            20) {
            ++run;
        } else if (run > 0) {
            break;
        }
    }
    return run;
}

TEST(OverlayPainter, PointIsAFixedSizeDot) {
    const QColor grey(128, 128, 128);
    const QImage plain = paintOn({Point{{100, 100}}}, 1.0, grey);
    const QImage zoomed = paintOn({Point{{20, 20}}}, 5.0, grey);

    const int plainWidth = differingRun(plain, 100, 80, grey);
    EXPECT_GE(plainWidth, 6);
    EXPECT_LE(plainWidth, 9);
    EXPECT_EQ(differingRun(zoomed, 100, 80, grey), plainWidth);
}

TEST(OverlayPainter, LaterOverlaysAreOnTop) {
    const QImage image = paintOn({Rectangle{{40, 40, 100, 100}}, Rectangle{{60, 60, 100, 100}, "", kRed}});

    const QColor overlap = image.pixelColor(100, 100);
    EXPECT_GT(overlap.red(), overlap.green());
}

TEST(OverlayPainter, ControlCharactersInALabelDrawOnOneLine) {
    EXPECT_EQ(qt_common::singleLineLabel("M\nR\tZ\xE2\x80\xA8\xC2\x85"), QStringLiteral("M R Z  "));

    const QImage plain = paintOn({Rectangle{{20, 40, 20, 20}, "MRZ"}});
    const QImage controls = paintOn({Rectangle{{20, 40, 20, 20}, "M\nR\tZ"}});
    EXPECT_EQ(tabHeight(controls, 20, 40), tabHeight(plain, 20, 40));
}

TEST(OverlayPainter, ShapesOutsideTheWindowAreSkipped) {
    std::vector<cv::Point2d> points;
    for (int i = 0; i < 1024; ++i) {
        points.emplace_back(900000.0 + i % 32, 900000.0 + i / 32.0);
    }

    const QImage image = paintOn({Polygon{points, "far"}}, 32.0);

    EXPECT_EQ(image.pixelColor(100, 100), QColor(Qt::black));
    EXPECT_EQ(image.pixelColor(0, 0), QColor(Qt::black));
}

TEST(OverlayPainter, HugeShapesAreClippedToTheWindow) {
    const QImage image = paintOn({Rectangle{{-1e6, -1e6, 2e6, 2e6}}}, 32.0);

    for (const auto& [x, y] : {std::pair{0, 0}, std::pair{199, 0}, std::pair{100, 100}, std::pair{0, 199}}) {
        EXPECT_TRUE(near(image.pixelColor(x, y), 30, 58, 30)) << x << "," << y;
    }
}

// The canvas's guide: a white image so the halo shows, at the identity transform.
QImage paintGuideOnWhite(const Polygon& guide) {
    QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    qt_common::paintOverlays(painter, {guide}, QTransform());
    painter.end();
    return image;
}

Polygon guideAt100(Colour colour) {
    return Polygon{.points = {{100, 100}, {300, 100}, {300, 250}, {100, 250}},
                   .label = "Document",
                   .colour = colour,
                   .style = Polygon::Style::Guide};
}

// P2: a 4 px line in its colour on a 6 px halo, which shows one device pixel each side; no fill and no tab.
TEST(OverlayPainter, GuideIsALineWithAHaloAndNoFill) {
    const Colour green{0x3D, 0xDC, 0x84};
    const QImage image = paintGuideOnWhite(guideAt100(green));

    EXPECT_TRUE(near(image.pixelColor(200, 100), green.r, green.g, green.b, 2))
        << image.pixelColor(200, 100).name().toStdString();
    EXPECT_LT(relativeLuminance(image.pixelColor(200, 97)), 0.35)
        << image.pixelColor(200, 97).name().toStdString(); // black at alpha 140 over white: 115
    EXPECT_EQ(image.pixelColor(200, 180), QColor(Qt::white));
    for (int y = 82; y <= 95; ++y) {
        for (int x = 76; x <= 95; ++x) {
            ASSERT_EQ(image.pixelColor(x, y), QColor(Qt::white)) << x << ", " << y;
        }
    }
    // Where a Standard polygon's "Document" tab is, over the outline's top-left corner.
    Polygon standard = guideAt100(green);
    standard.style = Polygon::Style::Standard;
    EXPECT_NE(paintGuideOnWhite(standard).pixelColor(104, 92), QColor(Qt::white));
    EXPECT_EQ(image.pixelColor(104, 92), QColor(Qt::white));
}

TEST(OverlayPainter, GuideHaloContrastOverWhite) {
    const QImage image = paintGuideOnWhite(guideAt100({0x3D, 0xDC, 0x84}));

    EXPECT_GE(contrast(image.pixelColor(200, 97), Qt::white), 3.0);
}

} // namespace
