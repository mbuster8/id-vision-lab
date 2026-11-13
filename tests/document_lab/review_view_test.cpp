#include "review_view.h"
#include "scan_style.h"

#include "palette_support.h"

#include <doc_vision/mrz.h>
#include <lab_core/frame_processor.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>

#include <gtest/gtest.h>

// A4: Review's bar above the canvas and its panel in the results' place (spec §3.4, screen 06).

namespace {

using document_lab::ReviewBar;
using document_lab::ReviewOverlays;
using document_lab::ReviewPanel;
using Outcome = document_lab::ReviewPanel::Outcome;

// ICAO 9303's specimen (Part 4, Appendix A).
const std::vector<std::string> kSpecimen{"P<UTOERIKSSON<<ANNA<MARIA<<<<<<<<<<<<<<<<<<<",
                                         "L898902C36UTO7408122F1204159ZE184226B<<<<<10"};

lab_core::MrzScan specimenScan() {
    lab_core::MrzScan scan;
    const auto mrz = doc_vision::parseMrz(kSpecimen);
    EXPECT_TRUE(mrz && mrz->valid());
    if (mrz) {
        scan.mrz = *mrz;
    }
    return scan;
}

QString textOf(const QWidget& widget, const char* name) {
    const auto* label = widget.findChild<QLabel*>(name);
    return label != nullptr ? label->text() : QStringLiteral("(no %1)").arg(QString::fromLatin1(name));
}

bool shown(const QWidget& widget, const char* name) {
    const auto* child = widget.findChild<QWidget*>(name);
    return child != nullptr && child->isVisibleTo(&widget);
}

QPushButton* button(const QWidget& widget, const char* name) { return widget.findChild<QPushButton*>(name); }

constexpr doc_vision::MrzDate kToday{2026, 11, 12};

TEST(ReviewPanel, RowsOfTheSpecimen) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    EXPECT_EQ(textOf(panel, "mrzTitle"), "MRZ");
    const std::vector<std::pair<const char*, QString>> rows{
        {"surname", "ERIKSSON"},    {"givenNames", "ANNA MARIA"},    {"documentNumber", "L898902C3"},
        {"nationality", "UTO"},     {"born", "12 Aug 1974"},         {"sex", "F"},
        {"expires", "15 Apr 2012"}, {"personalNumber", "ZE184226B"}, {"composite", "check digit"}};
    for (const auto& [key, value] : rows) {
        EXPECT_EQ(textOf(panel, (std::string(key) + "Value").c_str()), value) << key;
    }
    EXPECT_EQ(textOf(panel, "surnameLabel"), "Surname");
    EXPECT_EQ(textOf(panel, "givenNamesLabel"), "Given names");
    EXPECT_EQ(textOf(panel, "documentNumberLabel"), "Document no.");
    EXPECT_EQ(textOf(panel, "bornLabel"), "Born");
    EXPECT_EQ(textOf(panel, "expiresLabel"), "Expires");
    EXPECT_EQ(textOf(panel, "personalNumberLabel"), "Personal no.");
    EXPECT_EQ(textOf(panel, "compositeLabel"), "Composite");
    // The check digits' rows carry ✓; the others none.
    for (const char* key : {"documentNumber", "born", "expires", "personalNumber", "composite"}) {
        EXPECT_EQ(textOf(panel, (std::string(key) + "Check").c_str()), "✓") << key;
    }
    for (const char* key : {"surname", "givenNames", "nationality", "sex"}) {
        EXPECT_EQ(textOf(panel, (std::string(key) + "Check").c_str()), "") << key;
    }
    EXPECT_EQ(textOf(panel, "reviewNote"), "Nothing is saved or sent unless you choose it.");
    EXPECT_EQ(button(panel, "saveButton")->text(), "Save to captures");
    EXPECT_EQ(button(panel, "sendButton")->text(), "Send to the viewer");
    EXPECT_EQ(button(panel, "discardButton")->text(), "Discard");
    EXPECT_EQ(button(panel, "scanAnotherButton")->text(), "Scan another");
}

// A field the MRZ leaves empty (a '<' sex, no personal number) shows a dash, not a blank.
TEST(ReviewPanel, EmptyFieldsShowADash) {
    auto scan = specimenScan();
    scan.mrz.sex.clear();
    scan.mrz.optionalData.clear();
    ReviewPanel panel;
    panel.setScan(scan, kToday);
    EXPECT_EQ(textOf(panel, "sexValue"), "—");
    EXPECT_EQ(textOf(panel, "personalNumberValue"), "—");
}

// Only valid reads reach an accept, but a check that failed would show ✗ rather than ✓.
TEST(ReviewPanel, FailedCheckShowsACross) {
    auto scan = specimenScan();
    scan.mrz.checks[0].passed = false;
    ReviewPanel panel;
    panel.setScan(scan, kToday);
    EXPECT_EQ(textOf(panel, "documentNumberCheck"), "✗");
}

TEST(ReviewPanel, ExpiredWarningByToday) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    EXPECT_TRUE(shown(panel, "expiredWarning"));
    EXPECT_EQ(textOf(panel, "expiredWarning"),
              "Expired on 15 Apr 2012. A5 and later steps flag this; the scan itself still succeeded.");
    panel.setScan(specimenScan(), {2011, 1, 1});
    EXPECT_FALSE(shown(panel, "expiredWarning"));
    // Expiring today is not expired.
    panel.setScan(specimenScan(), {2012, 4, 15});
    EXPECT_FALSE(shown(panel, "expiredWarning"));
    panel.setScan(specimenScan(), {2012, 4, 16});
    EXPECT_TRUE(shown(panel, "expiredWarning"));
}

TEST(ReviewPanel, ButtonsSignal) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    QSignalSpy saved(&panel, &ReviewPanel::save);
    QSignalSpy sent(&panel, &ReviewPanel::send);
    QSignalSpy discarded(&panel, &ReviewPanel::discard);
    QSignalSpy another(&panel, &ReviewPanel::scanAnother);
    button(panel, "saveButton")->click();
    button(panel, "sendButton")->click();
    button(panel, "discardButton")->click();
    button(panel, "scanAnotherButton")->click();
    EXPECT_EQ(saved.count(), 1);
    EXPECT_EQ(sent.count(), 1);
    EXPECT_EQ(discarded.count(), 1);
    EXPECT_EQ(another.count(), 1);
}

TEST(ReviewPanel, SaveDoneDisables) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    auto* save = button(panel, "saveButton");
    panel.setSaveOutcome(Outcome::Done);
    EXPECT_EQ(save->text(), "Saved to captures ✓");
    EXPECT_FALSE(save->isEnabled());
    EXPECT_FALSE(shown(panel, "saveFailure"));

    panel.setScan(specimenScan(), kToday); // a new scan: back to Idle
    EXPECT_EQ(save->text(), "Save to captures");
    EXPECT_TRUE(save->isEnabled());

    panel.setSaveOutcome(Outcome::Failed, QStringLiteral("Could not save: the folder is read-only"));
    EXPECT_EQ(save->text(), "Save to captures");
    EXPECT_TRUE(save->isEnabled());
    EXPECT_TRUE(shown(panel, "saveFailure"));
    EXPECT_EQ(textOf(panel, "saveFailure"), "Could not save: the folder is read-only");
    panel.setSaveOutcome(Outcome::Done);
    EXPECT_FALSE(shown(panel, "saveFailure"));
}

TEST(ReviewPanel, SendBusyThenDone) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    auto* send = button(panel, "sendButton");
    panel.setSendOutcome(Outcome::Busy);
    EXPECT_EQ(send->text(), "Sending…");
    EXPECT_FALSE(send->isEnabled());
    panel.setSendOutcome(Outcome::Done);
    EXPECT_EQ(send->text(), "Sent to the viewer ✓");
    EXPECT_FALSE(send->isEnabled());
}

TEST(ReviewPanel, SendFailedSaysWhyAndEnables) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    auto* send = button(panel, "sendButton");
    panel.setSendOutcome(Outcome::Busy);
    panel.setSendOutcome(Outcome::Failed, QStringLiteral("the viewer changed"));
    EXPECT_EQ(send->text(), "Send to the viewer");
    EXPECT_TRUE(send->isEnabled());
    EXPECT_TRUE(shown(panel, "sendFailure"));
    EXPECT_EQ(textOf(panel, "sendFailure"), "the viewer changed");
    panel.setSendOutcome(Outcome::Busy);
    EXPECT_FALSE(shown(panel, "sendFailure"));
}

// A short window: the panel scrolls rather than making the window taller (a window that grew after it was
// shown redrew its canvas, seen on Windows), and its buttons stay reachable.
TEST(ReviewPanel, ShortPanelScrolls) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    EXPECT_LE(panel.minimumSizeHint().height(), 200);
    panel.resize(330, 300);
    panel.show();
    QApplication::processEvents();
    const auto* scroll = panel.findChild<QScrollArea*>();
    ASSERT_NE(scroll, nullptr);
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    EXPECT_TRUE(scroll->widget()->isAncestorOf(button(panel, "saveButton")));
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
}

TEST(ReviewPanel, IsAtLeast330Wide) {
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    EXPECT_GE(panel.minimumWidth(), 330);
}

TEST(ReviewPanel, DarkColours) {
    const test_support::PaletteGuard guard;
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    const auto* value = panel.findChild<QLabel*>("surnameValue");
    const auto* check = panel.findChild<QLabel*>("bornCheck");
    ASSERT_TRUE(value && check);
    EXPECT_EQ(value->palette().color(QPalette::WindowText), QColor(0x1E, 0x23, 0x29));
    EXPECT_EQ(check->palette().color(QPalette::WindowText), QColor(0x1E, 0x7D, 0x45));
    test_support::applyPalette(test_support::paletteWith(QColor(0x1E, 0x1E, 0x1E), QColor(0xF0, 0xF0, 0xF0)));
    EXPECT_EQ(value->palette().color(QPalette::WindowText), QColor(0xE8, 0xEA, 0xED));
    EXPECT_EQ(check->palette().color(QPalette::WindowText), QColor(0x3D, 0xDC, 0x84));
}

// The owner's camera check (A4): after a switch from dark to light, the panel stayed dark under light-mode
// text. QScrollArea::setWidget turned its content's autoFillBackground on, so the content painted the palette
// it started with over the panel's own card colour.
TEST(ReviewPanel, BackgroundFollowsAnAppearanceSwitch) {
    const test_support::PaletteGuard guard;
    test_support::applyPalette(test_support::paletteWith(QColor(0x1E, 0x1E, 0x1E), QColor(0xF0, 0xF0, 0xF0)));
    ReviewPanel panel;
    panel.setScan(specimenScan(), kToday);
    panel.resize(360, 640);
    panel.show();
    QApplication::processEvents();
    test_support::applyPalette(test_support::paletteWith(QColor(0xEC, 0xEC, 0xEC), QColor(0x1E, 0x1E, 0x1E)));
    QApplication::processEvents();

    QImage image(panel.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::magenta);
    panel.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    const QColor light = document_lab::reviewColours(false).cardBackground;
    EXPECT_EQ(image.pixelColor(8, 8), light); // in the content's padding, above the title
}

// A4.1, spec §6.7: a TD1 card's Review has a Back | Front switch; on the front, the band has no meaning.
TEST(ReviewBar, SwitchOnlyWithAFront) {
    ReviewBar bar;
    bar.resize(900, 40);
    bar.show();
    auto* back = bar.findChild<QPushButton*>("backSideButton");
    auto* front = bar.findChild<QPushButton*>("frontSideButton");
    ASSERT_TRUE(back && front);
    EXPECT_FALSE(front->isVisibleTo(&bar));
    bar.setHasFront(true);
    EXPECT_TRUE(front->isVisibleTo(&bar));
    EXPECT_EQ(bar.side(), document_lab::CardSide::Back);
    QSignalSpy changed(&bar, &ReviewBar::sideChanged);

    front->click();

    EXPECT_EQ(bar.side(), document_lab::CardSide::Front);
    EXPECT_EQ(changed.count(), 1);
    EXPECT_FALSE(bar.findChild<QCheckBox*>("bandBox")->isEnabled());
    EXPECT_FALSE(bar.overlays().band);
    bar.reset();
    EXPECT_EQ(bar.side(), document_lab::CardSide::Back);
    EXPECT_TRUE(bar.findChild<QCheckBox*>("bandBox")->isEnabled());
}

// TD1 and TD2 have optional data where TD3 has a personal number, and no check digit over it.
TEST(ReviewPanel, RowsForTd1) {
    ReviewPanel panel;
    lab_core::MrzScan scan;
    const auto mrz = doc_vision::parseMrz({"I<UTOD231458907<<<<<<<<<<<<<<<", "7408122F1204159UTO<<<<<<<<<<<6",
                                           "ERIKSSON<<ANNA<MARIA<<<<<<<<<<"});
    ASSERT_TRUE(mrz && mrz->valid());
    scan.mrz = *mrz;

    panel.setScan(scan, kToday);
    EXPECT_EQ(textOf(panel, "personalNumberLabel"), "Optional data");
    EXPECT_EQ(textOf(panel, "personalNumberCheck"), "");
    EXPECT_EQ(textOf(panel, "documentNumberValue"), "D23145890");

    panel.setScan(specimenScan(), kToday);
    EXPECT_EQ(textOf(panel, "personalNumberLabel"), "Personal no.");
}

TEST(ReviewBar, DefaultsAndToggles) {
    ReviewBar bar;
    EXPECT_EQ(textOf(bar, "reviewBarTitle"), "Captured frame");
    EXPECT_EQ(textOf(bar, "reviewBarLead"), "Show over it:");
    const ReviewOverlays defaults = bar.overlays();
    EXPECT_TRUE(defaults.outline);
    EXPECT_TRUE(defaults.band);
    EXPECT_FALSE(defaults.corners);
    EXPECT_FALSE(defaults.gates);

    const auto* zone = bar.findChild<QCheckBox*>("photoZoneBox");
    ASSERT_TRUE(zone);
    EXPECT_EQ(zone->text(), "Photo zone (A3.2)");
    EXPECT_FALSE(zone->isEnabled());

    QSignalSpy changed(&bar, &ReviewBar::overlaysChanged);
    auto* band = bar.findChild<QCheckBox*>("bandBox");
    auto* corners = bar.findChild<QCheckBox*>("cornersBox");
    ASSERT_TRUE(band && corners);
    EXPECT_EQ(band->text(), "MRZ band");
    EXPECT_EQ(corners->text(), "Corner numbers");
    EXPECT_EQ(bar.findChild<QCheckBox*>("outlineBox")->text(), "Document outline");
    EXPECT_EQ(bar.findChild<QCheckBox*>("gatesBox")->text(), "Gate values");
    band->click();
    corners->click();
    ASSERT_EQ(changed.count(), 2);
    const auto last = changed.last().at(0).value<ReviewOverlays>();
    EXPECT_TRUE(last.outline);
    EXPECT_FALSE(last.band);
    EXPECT_TRUE(last.corners);
    EXPECT_FALSE(last.gates);
    EXPECT_EQ(bar.overlays(), last);
}

// Every Review starts from the defaults, whatever the last one showed.
TEST(ReviewBar, ResetRestoresTheDefaults) {
    ReviewBar bar;
    bar.findChild<QCheckBox*>("outlineBox")->click();
    bar.findChild<QCheckBox*>("gatesBox")->click();
    QSignalSpy changed(&bar, &ReviewBar::overlaysChanged);
    bar.reset();
    EXPECT_EQ(bar.overlays(), ReviewOverlays{});
    EXPECT_EQ(changed.count(), 1);
}

TEST(GateValuesBox, ShowsTheRows) {
    document_lab::GateValuesBox box;
    box.setRows(
        {{"Fill", "62 %", lab_core::Finding::Tone::Good}, {"Glare", "0.4 %", lab_core::Finding::Tone::Good}});
    const auto labels = box.findChildren<QLabel*>();
    QStringList texts;
    for (const auto* label : labels) {
        texts << label->text();
    }
    EXPECT_TRUE(texts.contains("Fill: 62 %")) << texts.join(" | ").toStdString();
    EXPECT_TRUE(texts.contains("Glare: 0.4 %")) << texts.join(" | ").toStdString();
    box.setRows({});
    int visible = 0;
    for (const auto* label : box.findChildren<QLabel*>()) {
        visible += label->isVisibleTo(&box) ? 1 : 0;
    }
    EXPECT_EQ(visible, 0);
}

} // namespace
