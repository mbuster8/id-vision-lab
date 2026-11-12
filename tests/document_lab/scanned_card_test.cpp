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

// A4.1's D5 and D10: an ID card's words, and back to a passport's.
TEST(ScannedCard, IdCardWords) {
    ScannedCard card;
    card.setKind(lab_core::DocumentKind::IdCard);
    EXPECT_EQ(textOf(card, "scannedTitle"), "ID card scanned");
    EXPECT_EQ(textOf(card, "scannedHeadline"), "Now place the card on the chip reader");
    EXPECT_EQ(textOf(card, "scannedExplanation"), "Lay the card flat on the reader.");
    card.setKind(lab_core::DocumentKind::Passport);
    EXPECT_EQ(textOf(card, "scannedTitle"), "Passport scanned");
    EXPECT_EQ(textOf(card, "scannedHeadline"), "Now place the passport on the chip reader");
}

// The reader drawn with a card on it rather than a passport: the card is narrower than the passport's cover,
// so the illustration paints fewer pixels in the document's colour.
TEST(ScannedCard, DrawsACardOnTheReader) {
    const auto documentPixels = [](lab_core::DocumentKind kind) {
        ScannedCard card;
        card.setKind(kind);
        showAt(card, {1000, 760});
        auto* illustration = card.findChild<QWidget*>("readerIllustration");
        EXPECT_TRUE(illustration);
        if (illustration == nullptr) {
            return 0;
        }
        QImage image(illustration->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        illustration->render(&image);
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                count += image.pixelColor(x, y) == document_lab::kIllustrationPassport ? 1 : 0;
            }
        }
        return count;
    };
    const int passport = documentPixels(lab_core::DocumentKind::Passport);
    const int idCard = documentPixels(lab_core::DocumentKind::IdCard);
    EXPECT_GT(passport, 0);
    EXPECT_GT(idCard, 0);
    EXPECT_LT(idCard, passport);
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

// The owner's camera check (A4): on a narrow canvas the wrapped texts were cut off. A word-wrapped label
// added with an alignment gets its size hint's height, not the height its width needs; each gets the latter,
// and the explanation, narrower than the card, stays centred.
TEST(ScannedCard, WrappedTextsGetTheirWholeHeight) {
    ScannedCard card;
    showAt(card, {500, 400});
    const auto* body = card.findChild<QWidget*>("scannedCardBody");
    ASSERT_TRUE(body);
    for (const char* name : {"scannedTitle", "scannedHeadline", "scannedExplanation", "scannedNote"}) {
        const auto* label = card.findChild<QLabel*>(name);
        ASSERT_TRUE(label) << name;
        EXPECT_GE(label->height(), label->heightForWidth(label->width())) << name;
    }
    const auto* explanation = card.findChild<QLabel*>("scannedExplanation");
    EXPECT_LE(explanation->width(), 470);
    EXPECT_NEAR(explanation->mapTo(body, explanation->rect().center()).x(), body->rect().center().x(), 1);
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

// The owner's camera check (A4): around the card, inside its scroll area, the frame showed through dimmed
// only at the edges; the rest was a flat fill. QScrollArea::setWidget turns its widget's autoFillBackground
// on.
TEST(ScannedCard, DimsTheCanvasAroundTheCardToo) {
    ScannedCard card;
    showAt(card, {1000, 760});
    QImage image(card.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    card.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    const QColor expected = document_lab::over(document_lab::kScannedDim, Qt::white);
    const QColor beside = image.pixelColor(100, 380); // inside the scroll area, left of the 620 px card
    EXPECT_NEAR(beside.red(), expected.red(), 1);
    EXPECT_NEAR(beside.green(), expected.green(), 1);
    EXPECT_NEAR(beside.blue(), expected.blue(), 1);
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
