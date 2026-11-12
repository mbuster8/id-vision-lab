#include "scan_banner.h"
#include "scan_style.h"
#include "scan_words.h"

#include <doc_vision/guidance.h>
#include <doc_vision/quality_gates.h>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <gtest/gtest.h>

#include <optional>

namespace {

using doc_vision::Gate;
using doc_vision::Hint;
using doc_vision::Tone;
using document_lab::BannerContent;
using document_lab::WindowState;

int pixelsOf(const QImage& image, const QColor& colour) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += image.pixelColor(x, y) == colour ? 1 : 0;
        }
    }
    return count;
}

// As over the canvas: a child widget paints no background of its own, so render() is told not to either.
QImage rendered(QWidget& widget, QSize size) {
    widget.resize(size);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

TEST(ScanWords, WordsAreTheSpecs) {
    struct Row {
        Hint hint;
        Tone tone;
        bool card;
        const char* big;
        const char* small;
    };
    const Row rows[] = {
        {Hint::NoPage, Tone::Neutral, true, "Show the document",
         "the side with the photo, all of it in view"},
        {Hint::CutByEdge, Tone::Bad, false, "Move back", "the whole document must be in view"},
        {Hint::MoveToMiddle, Tone::Bad, false, "Move the document to the middle",
         "the whole document must be in view"},
        {Hint::TooFar, Tone::Bad, false, "Move closer", ""},
        {Hint::TooClose, Tone::Bad, false, "Move a little further away", ""},
        {Hint::Tilted, Tone::Bad, false, "Hold the document flat to the camera", ""},
        {Hint::Blurred, Tone::Bad, false, "Blurred: hold still", "or move a little closer or further away"},
        {Hint::Glare, Tone::Bad, false, "Glare on the document", "tilt it slightly away from the light"},
        {Hint::TooDark, Tone::Bad, false, "Too dark", "add more light"},
        {Hint::TooBright, Tone::Bad, false, "Too bright", "move out of direct light"},
        {Hint::Good, Tone::Good, false, "Good: hold still", ""},
    };
    for (const Row& row : rows) {
        const BannerContent content = document_lab::bannerFor(row.hint);
        EXPECT_EQ(content.tone, row.tone) << row.big;
        EXPECT_EQ(content.cardIcon, row.card) << row.big;
        EXPECT_EQ(content.bigLine, QString::fromUtf8(row.big));
        EXPECT_EQ(content.smallLine, QString::fromUtf8(row.small)) << row.big;
    }
}

// A3.3: the focus banners, word for word (spec §3.5).
TEST(ScanWords, FocusWords) {
    using document_lab::FocusBanner;
    EXPECT_EQ(document_lab::bannerFor(FocusBanner::Focusing),
              (BannerContent{Tone::Neutral, false, QStringLiteral("Hold still: focusing"), QString()}));
    EXPECT_EQ(document_lab::bannerFor(FocusBanner::CannotFocusThisClose),
              (BannerContent{Tone::Bad, false, QStringLiteral("This camera can't focus this close"),
                             QStringLiteral("Move the document a little further away")}));
    EXPECT_EQ(document_lab::bannerFor(FocusBanner::CannotMakeSharp),
              (BannerContent{Tone::Bad, false, QStringLiteral("This camera can't make the document sharp"),
                             QStringLiteral("Try more light, or another camera")}));
}

// A4: hints 11 and 12 (ADR-004's table; hint 12's small line as the owner chose, D2).
TEST(ScanWords, MrzHints) {
    EXPECT_EQ(
        document_lab::bannerFor(Hint::NoMrzBand),
        (BannerContent{Tone::Bad, false, QStringLiteral("Keep your fingers off the two lines at the bottom"),
                       QString()}));
    EXPECT_EQ(document_lab::bannerFor(Hint::CannotReadMrz),
              (BannerContent{Tone::Bad, false, QStringLiteral("Can't read the lines at the bottom"),
                             QStringLiteral("check for glare or fingers; only passports are read for now")}));
    EXPECT_EQ(document_lab::stateWord(Hint::NoMrzBand), "reading");
    EXPECT_EQ(document_lab::stateWord(Hint::CannotReadMrz), "reading");
}

TEST(ScanWords, WindowStates) {
    EXPECT_EQ(document_lab::bannerFor(WindowState::NoCamera),
              (BannerContent{Tone::Bad, false, "No camera found",
                             "Connect a camera, or open a video, an image or a folder"}));
#if defined(Q_OS_MACOS)
    const QString accessLine = QString::fromUtf8("Allow it in System Settings → Privacy & Security → Camera");
#elif defined(Q_OS_WIN)
    const QString accessLine = QString::fromUtf8("Allow it in Settings → Privacy & security → Camera");
#else
    const QString accessLine = QString::fromUtf8("Allow it in the system's privacy settings");
#endif
    EXPECT_EQ(document_lab::bannerFor(WindowState::AccessOff),
              (BannerContent{Tone::Bad, false, "Camera access is off", accessLine}));
    EXPECT_EQ(document_lab::bannerFor(WindowState::CameraStopped, "The camera was unplugged."),
              (BannerContent{Tone::Bad, false, "The camera stopped", "The camera was unplugged."}));
    EXPECT_EQ(
        document_lab::bannerFor(WindowState::Idle),
        (BannerContent{Tone::Neutral, true, "Start the camera", "or open a video, an image or a folder"}));
}

TEST(ScanWords, UnavailableWording) {
    const QString reinstall =
        "Reinstall the Document Lab, or rebuild it with the models: see the build guide";
    EXPECT_EQ(document_lab::bannerFor(WindowState::RuntimeMissing),
              (BannerContent{Tone::Bad, false, "ONNX Runtime could not be loaded", reinstall}));
    EXPECT_EQ(document_lab::bannerFor(WindowState::ModelMissing),
              (BannerContent{Tone::Bad, false, "The document model is missing", reinstall}));
}

TEST(ScanWords, StateWords) {
    EXPECT_EQ(document_lab::stateWord(Hint::NoPage), "searching");
    EXPECT_EQ(document_lab::stateWord(Hint::CutByEdge), "adjust (in view)");
    EXPECT_EQ(document_lab::stateWord(Hint::MoveToMiddle), "adjust (in view)");
    EXPECT_EQ(document_lab::stateWord(Hint::TooFar), "adjust (size)");
    EXPECT_EQ(document_lab::stateWord(Hint::TooClose), "adjust (size)");
    EXPECT_EQ(document_lab::stateWord(Hint::Tilted), "adjust (flat)");
    EXPECT_EQ(document_lab::stateWord(Hint::Blurred), "adjust (sharpness)");
    EXPECT_EQ(document_lab::stateWord(Hint::Glare), "adjust (glare)");
    EXPECT_EQ(document_lab::stateWord(Hint::TooDark), "adjust (light)");
    EXPECT_EQ(document_lab::stateWord(Hint::TooBright), "adjust (light)");
    EXPECT_EQ(document_lab::stateWord(Hint::Good), "good");
}

TEST(ScanWords, ChipLabels) {
    EXPECT_EQ(document_lab::chipLabel(Gate::InView), "In view");
    EXPECT_EQ(document_lab::chipLabel(Gate::Size), "Size");
    EXPECT_EQ(document_lab::chipLabel(Gate::Flat), "Flat");
    EXPECT_EQ(document_lab::chipLabel(Gate::Sharp), "Sharp");
    EXPECT_EQ(document_lab::chipLabel(Gate::Glare), "No glare");
    EXPECT_EQ(document_lab::chipLabel(Gate::Light), "Light");
}

TEST(ScanBanner, FitShrinksThenElides) {
    const QFont font = document_lab::bigLineFont(QWidget().font());
    ASSERT_EQ(font.pixelSize(), 32);
    const QString text = "Move the document to the middle";
    QFont at26 = font;
    at26.setPixelSize(26);
    const int width26 = QFontMetrics(at26).horizontalAdvance(text);

    const document_lab::FittedLine wide = document_lab::fitBigLine(font, text, 2000);
    const document_lab::FittedLine middle = document_lab::fitBigLine(font, text, width26);
    const document_lab::FittedLine narrow = document_lab::fitBigLine(font, text, 120);

    EXPECT_EQ(wide.pixelSize, 32);
    EXPECT_EQ(wide.text, text);
    EXPECT_EQ(middle.pixelSize, 26);
    EXPECT_EQ(middle.text, text);
    EXPECT_EQ(narrow.pixelSize, 22);
    EXPECT_TRUE(narrow.text.endsWith(QChar(0x2026))) << narrow.text.toStdString();
}

TEST(ScanBanner, BannerPaintsItsTone) {
    QWidget canvas;
    document_lab::ScanBanner banner(&canvas);
    banner.setContent(document_lab::bannerFor(Hint::Blurred));
    EXPECT_GE(pixelsOf(rendered(banner, {900, 200}), QColor(0xFF, 0x6B, 0x5E)), 50);

    banner.setContent(document_lab::bannerFor(Hint::Good));
    EXPECT_GE(pixelsOf(rendered(banner, {900, 200}), QColor(0x3D, 0xDC, 0x84)), 50);
    EXPECT_EQ(banner.fittedBigLine().pixelSize, 32);
}

TEST(ScanBanner, NoContentHidesIt) {
    QWidget canvas;
    document_lab::ScanBanner banner(&canvas);
    banner.setContent(document_lab::bannerFor(Hint::Good));
    EXPECT_FALSE(banner.isHidden());

    banner.setContent(std::nullopt);

    EXPECT_TRUE(banner.isHidden());
    EXPECT_FALSE(banner.content());
}

TEST(ScanBanner, NarrowBannerShrinksItsBigLine) {
    QWidget canvas;
    document_lab::ScanBanner banner(&canvas);
    banner.setContent(document_lab::bannerFor(Hint::MoveToMiddle));

    rendered(banner, {360, 260});

    EXPECT_LT(banner.fittedBigLine().pixelSize, 32);
}

TEST(GateChips, ChipsHideWithoutAPage) {
    QWidget canvas;
    document_lab::GateChips chips(&canvas);
    chips.setStates({.visible = true, .passed = {true, true, true, true, true, true}});
    EXPECT_FALSE(chips.isHidden());

    chips.setStates({});

    EXPECT_TRUE(chips.isHidden());
}

TEST(GateChips, ChipsShowCrossForAFailedGate) {
    QWidget canvas;
    document_lab::GateChips chips(&canvas);
    chips.setStates({.visible = true, .passed = {true, true, true, true, true, true}});
    const QImage allPass = rendered(chips, {700, 80});
    chips.setStates({.visible = true, .passed = {true, true, true, false, true, true}});
    const QImage sharpFails = rendered(chips, {700, 80});

    // A 14 px label and a thin cross have few pixels of the exact colour, fewer still in Windows' Segoe UI
    // (16 on CI); what matters is that the red is there only when a gate fails.
    EXPECT_EQ(pixelsOf(allPass, QColor(0xFF, 0x6B, 0x5E)), 0);
    EXPECT_GE(pixelsOf(sharpFails, QColor(0xFF, 0x6B, 0x5E)), 8);
}

TEST(GateChips, ChipsWrapWhenNarrow) {
    QWidget canvas;
    document_lab::GateChips chips(&canvas);
    chips.setStates({.visible = true, .passed = {true, true, true, true, true, true}});

    EXPECT_TRUE(chips.hasHeightForWidth());
    EXPECT_GT(chips.heightForWidth(220), chips.heightForWidth(1000));
}

TEST(ScanBanner, TransparentToTheMouse) {
    document_lab::ScanBanner banner;
    document_lab::GateChips chips;

    EXPECT_TRUE(banner.testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_TRUE(chips.testAttribute(Qt::WA_TransparentForMouseEvents));
}

// P21: the gaps are the widgets' own transparent margins: 28 px above the banner's panel, 22 px below the
// chips.
TEST(ScanBanner, GapsAreTheWidgetsMargins) {
    QWidget canvas;
    document_lab::ScanBanner banner(&canvas);
    banner.setContent(document_lab::bannerFor(Hint::Good));
    document_lab::GateChips chips(&canvas);
    chips.setStates({.visible = true, .passed = {true, true, true, true, true, true}});

    const QSize bannerSize = banner.sizeHint();
    const QImage bannerImage = rendered(banner, bannerSize);
    const QSize chipsSize = chips.sizeHint();
    const QImage chipsImage = rendered(chips, chipsSize);

    for (int x = 0; x < bannerSize.width(); ++x) {
        ASSERT_EQ(bannerImage.pixelColor(x, 27), QColor(Qt::white)) << x;
    }
    for (int x = 0; x < chipsSize.width(); ++x) {
        ASSERT_EQ(chipsImage.pixelColor(x, chipsSize.height() - 22), QColor(Qt::white)) << x;
    }
    EXPECT_NE(bannerImage.pixelColor(bannerSize.width() / 2, 29), QColor(Qt::white));
    EXPECT_NE(chipsImage.pixelColor(chipsSize.width() / 2, chipsSize.height() - 24), QColor(Qt::white));
}

} // namespace
