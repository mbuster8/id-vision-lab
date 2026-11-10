#include "outline_icons.h"
#include "palette_support.h"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPalette>

#include <gtest/gtest.h>

#include <initializer_list>

namespace {

using qt_common::makeOutlineIcon;
using qt_common::OutlineIcon;

constexpr std::initializer_list<OutlineIcon> kAllShapes = {
    OutlineIcon::Pencil,      OutlineIcon::Duplicate, OutlineIcon::Trash,     OutlineIcon::Pause,
    OutlineIcon::Play,        OutlineIcon::Stop,      OutlineIcon::ChevronUp, OutlineIcon::ChevronDown,
    OutlineIcon::Copy,        OutlineIcon::Clear,     OutlineIcon::Search,    OutlineIcon::Options,
    OutlineIcon::Info,        OutlineIcon::Warning,   OutlineIcon::Error,     OutlineIcon::Folder,
    OutlineIcon::Send,        OutlineIcon::Camera,    OutlineIcon::EyeOff,    OutlineIcon::StepBack,
    OutlineIcon::StepForward, OutlineIcon::Back10,    OutlineIcon::Forward10, OutlineIcon::Restart,
    OutlineIcon::Loop,        OutlineIcon::Fast,      OutlineIcon::Record,    OutlineIcon::Overlays,
    OutlineIcon::Tick,        OutlineIcon::Cross};

QImage render(OutlineIcon shape) { return makeOutlineIcon(shape).pixmap(24, 24).toImage(); }

int countDrawnPixels(const QImage& image) {
    int drawn = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            drawn += image.pixelColor(x, y).alpha() > 0 ? 1 : 0;
        }
    }
    return drawn;
}

// True when at least one pixel is fully opaque and every fully opaque pixel has the colour.
bool strokedIn(const QImage& image, const QColor& colour) {
    int opaque = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 255) {
                if (pixel.rgb() != colour.rgb()) {
                    return false;
                }
                ++opaque;
            }
        }
    }
    return opaque > 0;
}

void setWindowText(const QColor& colour) {
    QPalette palette = QGuiApplication::palette();
    palette.setColor(QPalette::WindowText, colour);
    QGuiApplication::setPalette(palette);
}

TEST(OutlineIcons, EveryShapeDrawsSomething) {
    for (const OutlineIcon shape : kAllShapes) {
        EXPECT_GT(countDrawnPixels(render(shape)), 10) << "shape " << static_cast<int>(shape);
    }
}

TEST(OutlineIcons, FollowTheApplicationPalette) {
    const test_support::PaletteGuard guard;

    setWindowText(Qt::red);
    EXPECT_TRUE(strokedIn(render(OutlineIcon::Pencil), Qt::red));

    setWindowText(Qt::blue);
    EXPECT_TRUE(strokedIn(render(OutlineIcon::Pencil), Qt::blue));
}

// A toggle that is on (Loop, Overlays) draws in the accent colour, so on and off differ without a frame.
TEST(OutlineIcons, OnStateUsesTheAccent) {
    const test_support::PaletteGuard guard;
    QPalette palette = QGuiApplication::palette();
    palette.setColor(QPalette::WindowText, Qt::red);
    palette.setColor(QPalette::Highlight, Qt::green);
    QGuiApplication::setPalette(palette);
    const QIcon icon = makeOutlineIcon(OutlineIcon::Loop);

    EXPECT_TRUE(strokedIn(icon.pixmap(QSize(24, 24), QIcon::Normal, QIcon::On).toImage(), Qt::green));
    EXPECT_TRUE(strokedIn(icon.pixmap(QSize(24, 24), QIcon::Normal, QIcon::Off).toImage(), Qt::red));
}

// ▶ green and ■ red whatever the palette; grey, as every disabled icon, when disabled.
TEST(OutlineIcons, ColouredIconsKeepTheirColour) {
    const test_support::PaletteGuard guard;
    QPalette palette = QGuiApplication::palette();
    palette.setColor(QPalette::WindowText, Qt::blue);
    palette.setColor(QPalette::PlaceholderText, Qt::gray); // opaque: Fusion's own is half transparent
    QGuiApplication::setPalette(palette);
    const QIcon play = qt_common::makeColouredIcon(OutlineIcon::Play, qt_common::kGo);

    EXPECT_TRUE(strokedIn(play.pixmap(QSize(24, 24)).toImage(), qt_common::kGo));
    EXPECT_GT(countDrawnPixels(play.pixmap(QSize(24, 24)).toImage()), 60); // filled, not only stroked
    EXPECT_TRUE(strokedIn(play.pixmap(QSize(24, 24), QIcon::Disabled).toImage(), QColor(Qt::gray)));
    EXPECT_TRUE(strokedIn(
        qt_common::makeColouredIcon(OutlineIcon::Record, qt_common::kHalt).pixmap(QSize(24, 24)).toImage(),
        qt_common::kHalt));
}

} // namespace
