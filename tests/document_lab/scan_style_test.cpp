#include "scan_style.h"

#include <doc_vision/guidance.h>
#include <lab_core/frame_processor.h>

#include <QColor>

#include <gtest/gtest.h>

namespace {

using doc_vision::Tone;

TEST(ScanStyle, ContrastRatioIsWcags) {
    EXPECT_NEAR(document_lab::contrastRatio(Qt::black, Qt::white), 21.0, 1e-9);
    EXPECT_NEAR(document_lab::contrastRatio(Qt::white, Qt::black), 21.0, 1e-9);
    EXPECT_NEAR(document_lab::contrastRatio(QColor(0x77, 0x77, 0x77), Qt::white), 4.48, 0.01);
}

TEST(ScanStyle, OverCompositesByAlpha) {
    const QColor half = document_lab::over(QColor(0, 0, 0, 128), Qt::white);
    EXPECT_EQ(half.alpha(), 255);
    EXPECT_NEAR(half.red(), 127, 1);
    EXPECT_EQ(document_lab::over(QColor(10, 20, 30), Qt::white), QColor(10, 20, 30));
}

// Every banner and chip colour on the panel over a white page, the panel's tones on its light and dark
// backgrounds: at least WCAG's 4.5:1.
TEST(ScanStyle, ContrastOfEveryPair) {
    const QColor panel = document_lab::over(document_lab::kBannerPanel, Qt::white);
    for (const Tone tone : {Tone::Neutral, Tone::Bad, Tone::Good}) {
        const document_lab::ToneColours colours = document_lab::bannerColours(tone);
        EXPECT_GE(document_lab::contrastRatio(colours.big, panel), 4.5) << colours.big.name().toStdString();
        EXPECT_GE(document_lab::contrastRatio(colours.small, panel), 4.5)
            << colours.small.name().toStdString();
    }
    EXPECT_EQ(document_lab::bannerColours(Tone::Good).big, document_lab::kGuideGood);
    EXPECT_EQ(document_lab::bannerColours(Tone::Bad).big, document_lab::kGuideBad);
    using FindingTone = lab_core::Finding::Tone;
    const QColor darkPanel(0x24, 0x28, 0x2E);
    for (const FindingTone tone : {FindingTone::Good, FindingTone::Bad}) {
        EXPECT_GE(document_lab::contrastRatio(document_lab::toneColour(tone, false), Qt::white), 4.5);
        EXPECT_GE(document_lab::contrastRatio(document_lab::toneColour(tone, true), darkPanel), 4.5);
    }
}

TEST(ScanStyle, BannerColoursAreTheSpecs) {
    EXPECT_EQ(document_lab::bannerColours(Tone::Neutral).big, QColor(0xFF, 0xFF, 0xFF));
    EXPECT_EQ(document_lab::bannerColours(Tone::Neutral).small, QColor(0xC9, 0xCE, 0xD6));
    EXPECT_EQ(document_lab::bannerColours(Tone::Bad).big, QColor(0xFF, 0x6B, 0x5E));
    EXPECT_EQ(document_lab::bannerColours(Tone::Bad).small, QColor(0xF2, 0xC7, 0xC2));
    EXPECT_EQ(document_lab::bannerColours(Tone::Good).big, QColor(0x3D, 0xDC, 0x84));
    EXPECT_EQ(document_lab::bannerColours(Tone::Good).small, QColor(0xC7, 0xEE, 0xD8));
    EXPECT_EQ(document_lab::kBannerPanel, QColor(8, 10, 13, 217));
}

// A4: spec §3.5's table, pair by pair, in both appearances: each at least 4.5:1, each the table's ratio.
TEST(ScanStyle, ReviewPairsMeetFourPointFive) {
    using document_lab::contrastRatio;
    const document_lab::ReviewColours light = document_lab::reviewColours(false);
    const document_lab::ReviewColours dark = document_lab::reviewColours(true);
    struct Pair {
        const char* what;
        QColor foreground;
        QColor background;
        double ratio;
    };
    const Pair pairs[] = {
        {"light title", light.title, light.cardBackground, 5.15},
        {"light text on the pill", light.text, light.pillBackground, 14.35},
        {"light secondary on the pill", light.secondary, light.pillBackground, 6.64},
        {"light discard", light.discardText, light.discardBackground, 7.10},
        {"light warning", light.warnText, light.warnBackground, 6.80},
        {"light link", light.link, light.cardBackground, 7.34},
        {"light check", light.checkMark, light.checkCircle, 4.60},
        {"dark title", dark.title, dark.cardBackground, 8.30},
        {"dark text", dark.text, dark.cardBackground, 12.29},
        {"dark secondary", dark.secondary, dark.cardBackground, 6.84},
        {"dark secondary on the pill", dark.secondary, dark.pillBackground, 5.88},
        {"dark text on the pill", dark.text, dark.pillBackground, 10.57},
        {"dark discard on the pill", dark.discardText, dark.pillBackground, 5.58},
        {"dark discard", dark.discardText, dark.discardBackground, 6.43},
        {"dark warning", dark.warnText, dark.warnBackground, 8.85},
        {"dark link", dark.link, dark.cardBackground, 7.09},
        {"dark check", dark.checkMark, dark.checkCircle, 6.95},
        {"primary", light.primaryText, light.primary, 4.55},
        {"primary, dark", dark.primaryText, dark.primary, 4.55},
        {"amber band", document_lab::kBandAmber, document_lab::over(document_lab::kBannerPanel, Qt::white),
         7.94},
    };
    for (const Pair& pair : pairs) {
        const double ratio = contrastRatio(pair.foreground, pair.background);
        EXPECT_GE(ratio, 4.5) << pair.what;
        EXPECT_NEAR(ratio, pair.ratio, 0.01) << pair.what;
    }
}

TEST(ScanStyle, ReviewColoursAreTheSpecs) {
    const document_lab::ReviewColours light = document_lab::reviewColours(false);
    const document_lab::ReviewColours dark = document_lab::reviewColours(true);
    EXPECT_EQ(light.title, QColor(0x1E, 0x7D, 0x45));
    EXPECT_EQ(dark.title, QColor(0x3D, 0xDC, 0x84));
    EXPECT_EQ(light.primary, QColor(0x2F, 0x6F, 0xED));
    EXPECT_EQ(light.buttonBorder, QColor(0xC6, 0xCB, 0xD2));
    EXPECT_EQ(dark.buttonBorder, QColor(0x4A, 0x50, 0x58));
    EXPECT_EQ(light.discardBorder, QColor(0xE3, 0xB2, 0xAC));
    EXPECT_EQ(dark.discardBorder, QColor(0x6B, 0x3A, 0x35));
    EXPECT_EQ(document_lab::kScannedDim, QColor(8, 10, 13, 158)); // rgba(8, 10, 13, 0.62)
    EXPECT_EQ(document_lab::kProgressFilled, QColor(0x3D, 0xDC, 0x84));
    EXPECT_EQ(document_lab::kProgressEmpty, QColor(0x3E, 0x4A, 0x44));
}

} // namespace
