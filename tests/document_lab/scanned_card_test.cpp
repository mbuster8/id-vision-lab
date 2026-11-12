#include "scan_style.h"
#include "scanned_card.h"

#include "palette_support.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>

#include <gtest/gtest.h>

// A4: screen 04's card over the dimmed captured frame (spec §3.3).

namespace {

using document_lab::ScannedCard;

QString textOf(const QWidget& widget, const char* name) {
    const auto* label = widget.findChild<QLabel*>(name);
    return label != nullptr ? label->text() : QStringLiteral("(no %1)").arg(QString::fromLatin1(name));
}

QPushButton* button(const QWidget& widget, const char* name) { return widget.findChild<QPushButton*>(name); }

// Lays the card out at `size`, as it is when shown over the canvas.
void showAt(ScannedCard& card, QSize size) {
    card.resize(size);
    card.show();
    QApplication::processEvents();
}

TEST(ScannedCard, TextsAsDrawn) {
    ScannedCard card;
    EXPECT_EQ(textOf(card, "scannedTitle"), "Passport scanned");
    EXPECT_EQ(textOf(card, "scannedHeadline"), "Now place the passport on the chip reader");
    EXPECT_EQ(textOf(card, "scannedExplanation"), "Close it and lay the cover flat on the reader. For most "
                                                  "passports the chip is in the cover or the data page.");
    EXPECT_EQ(textOf(card, "readerLine"), "Chip reading arrives in A5");
    EXPECT_EQ(textOf(card, "scannedNote"),
              "The captured frame is kept until you choose: save, send or discard it after the chip step.");
    ASSERT_TRUE(button(card, "skipChipButton"));
    ASSERT_TRUE(button(card, "scanAgainButton"));
    EXPECT_EQ(button(card, "skipChipButton")->text(), "Skip the chip");
    EXPECT_EQ(button(card, "scanAgainButton")->text(), "Scan again");
}

TEST(ScannedCard, ButtonsSignal) {
    ScannedCard card;
    QSignalSpy skipped(&card, &ScannedCard::skipChip);
    QSignalSpy again(&card, &ScannedCard::scanAgain);
    button(card, "skipChipButton")->click();
    button(card, "scanAgainButton")->click();
    EXPECT_EQ(skipped.count(), 1);
    EXPECT_EQ(again.count(), 1);
}

TEST(ScannedCard, CardIs620Wide) {
    ScannedCard card;
    showAt(card, {1000, 760});
    const auto* body = card.findChild<QWidget*>("scannedCardBody");
    ASSERT_TRUE(body);
    EXPECT_EQ(body->width(), 620);
}

// A canvas smaller than the card: it narrows, its texts wrap, and it scrolls rather than hiding its buttons.
TEST(ScannedCard, NarrowCanvasKeepsTheButtonsReachable) {
    ScannedCard card;
    showAt(card, {500, 400});
    const auto* body = card.findChild<QWidget*>("scannedCardBody");
    const auto* scroll = card.findChild<QScrollArea*>();
    ASSERT_TRUE(body && scroll);
    EXPECT_LE(body->width(), 500 - 2 * 16);
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    EXPECT_TRUE(scroll->widget()->isAncestorOf(button(card, "skipChipButton")));
    EXPECT_TRUE(button(card, "skipChipButton")->isVisibleTo(&card));
}

TEST(ScannedCard, DarkColours) {
    const test_support::PaletteGuard guard;
    ScannedCard card;
    const auto* title = card.findChild<QLabel*>("scannedTitle");
    ASSERT_TRUE(title);
    EXPECT_EQ(title->palette().color(QPalette::WindowText), QColor(0x1E, 0x7D, 0x45));
    test_support::applyPalette(test_support::paletteWith(QColor(0x1E, 0x1E, 0x1E), QColor(0xF0, 0xF0, 0xF0)));
    EXPECT_EQ(title->palette().color(QPalette::WindowText), QColor(0x3D, 0xDC, 0x84));
}

TEST(ScannedCard, DimsTheCanvas) {
    ScannedCard card;
    showAt(card, {1000, 760});
    QImage image(card.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    card.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    const QColor expected = document_lab::over(document_lab::kScannedDim, Qt::white);
    const QColor corner = image.pixelColor(4, 4);
    EXPECT_NEAR(corner.red(), expected.red(), 1);
    EXPECT_NEAR(corner.green(), expected.green(), 1);
    EXPECT_NEAR(corner.blue(), expected.blue(), 1);
}

} // namespace
