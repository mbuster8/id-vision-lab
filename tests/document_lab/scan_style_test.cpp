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

} // namespace
