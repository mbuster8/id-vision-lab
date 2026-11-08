#include "display_settings_control.h"

#include "viewer_core/display_settings.h"

#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QString>
#include <QTest>
#include <QToolButton>
#include <QWidget>

#include <gtest/gtest.h>

#include <vector>

namespace {

using viewer_core::ColourMap;
using viewer_core::DisplayRange;
using viewer_core::DisplaySettings;
using viewer_core::RangeMode;

QAction* actionTitled(const QMenu* menu, const QString& title) {
    for (QAction* action : menu->actions()) {
        if (action->text().remove(u'&') == title) {
            return action;
        }
    }
    return nullptr;
}

QString checkedTitle(const QMenu* menu) {
    for (const QAction* action : menu->actions()) {
        if (action->isChecked()) {
            return action->text().remove(u'&');
        }
    }
    return {};
}

// A shown control that keeps what it emits.
class DisplaySettingsControlTest : public testing::Test {
protected:
    void SetUp() override {
        QObject::connect(&m_control, &DisplaySettingsControl::settingsChanged,
                         [this](const DisplaySettings& settings) { m_emitted.push_back(settings); });
        m_control.setAutoRangeProvider([this] { return m_autoRange; });
        m_control.setState(DisplaySettings{}, true, false);
        m_control.show();
    }

    QToolButton* button(const char* name) {
        return m_control.findChild<QToolButton*>(QString::fromLatin1(name));
    }
    QWidget* popup() { return m_control.findChild<QWidget*>(QStringLiteral("fixedRangePopup")); }
    QLineEdit* field(const char* name) { return m_control.findChild<QLineEdit*>(QString::fromLatin1(name)); }
    QPushButton* applyButton() {
        return m_control.findChild<QPushButton*>(QStringLiteral("applyFixedRange"));
    }

    void openFixed() {
        actionTitled(m_control.rangeMenu(), QStringLiteral("Fixed…"))->trigger();
        ASSERT_NE(popup(), nullptr);
        ASSERT_TRUE(popup()->isVisible());
    }

    void type(const char* name, const QString& text) {
        field(name)->selectAll();
        QTest::keyClicks(field(name), text);
    }

    DisplaySettingsControl m_control;
    DisplayRange m_autoRange{10, 20};
    std::vector<DisplaySettings> m_emitted;
};

TEST_F(DisplaySettingsControlTest, StartsOnAutoAndGrey) {
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: Auto ▾"));
    EXPECT_EQ(button("coloursButton")->text(), QStringLiteral("Colours: Grey ▾"));
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Auto"));
    EXPECT_EQ(checkedTitle(m_control.coloursMenu()), QStringLiteral("Grey"));
    QStringList ranges; // as listed: HDR only for an HDR image
    for (const QAction* action : m_control.rangeMenu()->actions()) {
        if (action->isVisible()) {
            ranges << action->text().remove(u'&');
        }
    }
    EXPECT_EQ(ranges,
              (QStringList{QStringLiteral("Auto"), QStringLiteral("Hold"), QStringLiteral("Fixed…")}));
    EXPECT_TRUE(m_emitted.empty());
}

TEST_F(DisplaySettingsControlTest, ButtonsOpenTheirMenus) {
    button("rangeButton")->click();
    EXPECT_TRUE(m_control.rangeMenu()->isVisible());
    m_control.rangeMenu()->hide();

    button("coloursButton")->click();
    EXPECT_TRUE(m_control.coloursMenu()->isVisible());
    m_control.coloursMenu()->hide();
}

TEST_F(DisplaySettingsControlTest, HoldCapturesTheProvidedRange) {
    actionTitled(m_control.rangeMenu(), QStringLiteral("Hold"))->trigger();

    ASSERT_EQ(m_emitted.size(), 1u);
    EXPECT_EQ(m_emitted.back(), (DisplaySettings{.mode = RangeMode::Hold, .range = {10, 20}}));
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: Hold ▾"));
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Hold"));

    actionTitled(m_control.rangeMenu(), QStringLiteral("Auto"))->trigger();
    ASSERT_EQ(m_emitted.size(), 2u);
    EXPECT_EQ(m_emitted.back(), DisplaySettings{});
}

TEST_F(DisplaySettingsControlTest, FixedRefusesAnEmptyRange) {
    openFixed();
    EXPECT_EQ(field("fixedMinimum")->text(), QStringLiteral("10")); // the current range
    EXPECT_EQ(field("fixedMaximum")->text(), QStringLiteral("20"));
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Auto")); // until applied

    type("fixedMinimum", QStringLiteral("5"));
    type("fixedMaximum", QStringLiteral("5"));
    EXPECT_FALSE(applyButton()->isEnabled());
    type("fixedMaximum", QStringLiteral("3"));
    EXPECT_FALSE(applyButton()->isEnabled());
    type("fixedMaximum", QStringLiteral(""));
    EXPECT_FALSE(applyButton()->isEnabled());
    type("fixedMinimum", QStringLiteral("-0.5"));
    type("fixedMaximum", QStringLiteral("1e3"));
    EXPECT_TRUE(applyButton()->isEnabled());
    EXPECT_TRUE(m_emitted.empty());

    applyButton()->click();

    ASSERT_EQ(m_emitted.size(), 1u);
    EXPECT_EQ(m_emitted.back(), (DisplaySettings{.mode = RangeMode::Fixed, .range = {-0.5, 1000}}));
    EXPECT_FALSE(popup()->isVisible());
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: Fixed ▾"));
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Fixed…"));
}

TEST_F(DisplaySettingsControlTest, EscapeLeavesTheRangeAsItWas) {
    openFixed();

    QTest::keyClick(field("fixedMinimum"), Qt::Key_Escape);

    EXPECT_FALSE(popup()->isVisible());
    EXPECT_TRUE(m_emitted.empty());
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Auto"));
}

TEST_F(DisplaySettingsControlTest, ColoursOnlyForSingleChannel) {
    actionTitled(m_control.coloursMenu(), QStringLiteral("Turbo"))->trigger();
    ASSERT_EQ(m_emitted.size(), 1u);
    EXPECT_EQ(m_emitted.back(), DisplaySettings{.colours = ColourMap::Turbo});
    EXPECT_EQ(button("coloursButton")->text(), QStringLiteral("Colours: Turbo ▾"));

    m_control.setState(DisplaySettings{}, false, false);

    EXPECT_TRUE(button("coloursButton")->isHidden());
    for (const QAction* action : m_control.coloursMenu()->actions()) {
        EXPECT_FALSE(action->isEnabled());
    }
    m_control.setState(DisplaySettings{.colours = ColourMap::Viridis}, true, false);
    EXPECT_FALSE(button("coloursButton")->isHidden());
    EXPECT_EQ(button("coloursButton")->text(), QStringLiteral("Colours: Viridis ▾"));
    EXPECT_EQ(checkedTitle(m_control.coloursMenu()), QStringLiteral("Viridis"));
    EXPECT_EQ(m_emitted.size(), 1u); // setState is quiet
}

// (F6) The control sits at the info strip's right end; the popup opens right-aligned under the Range button
// and stays inside the window, instead of running off its right edge.
TEST(DisplaySettingsControlPopup, FixedPopupStaysInsideTheWindow) {
    QWidget window;
    window.resize(600, 300);
    auto* control = new DisplaySettingsControl(&window);
    control->setState(DisplaySettings{}, false, false); // the Range button alone: narrower than the popup
    control->adjustSize();
    control->move(window.width() - control->width(), 0);
    window.show();

    actionTitled(control->rangeMenu(), QStringLiteral("Fixed…"))->trigger();

    const QWidget* popup = control->findChild<QWidget*>(QStringLiteral("fixedRangePopup"));
    ASSERT_NE(popup, nullptr);
    ASSERT_GT(popup->width(), control->width());
    EXPECT_TRUE(window.geometry().contains(popup->frameGeometry()))
        << popup->frameGeometry().x() << "," << popup->frameGeometry().y() << " " << popup->width() << "x"
        << popup->height();
}

// (R3) Per frame, quietly: a live channel can change its channel count from one frame to the next.
TEST_F(DisplaySettingsControlTest, SetFrameKindOnlyShowsOrHidesColoursAndHdr) {
    actionTitled(m_control.rangeMenu(), QStringLiteral("Fixed…"))->trigger();

    m_control.setFrameKind(false, false);
    EXPECT_TRUE(button("coloursButton")->isHidden());
    EXPECT_FALSE(actionTitled(m_control.coloursMenu(), QStringLiteral("Turbo"))->isEnabled());
    m_control.setFrameKind(true, true);
    EXPECT_FALSE(button("coloursButton")->isHidden());
    EXPECT_TRUE(actionTitled(m_control.rangeMenu(), QStringLiteral("HDR"))->isVisible());

    EXPECT_TRUE(popup()->isVisible()); // left open, unlike setState
    EXPECT_TRUE(m_emitted.empty());
}

TEST_F(DisplaySettingsControlTest, HdrOnlyWhenAvailable) {
    QAction* hdr = actionTitled(m_control.rangeMenu(), QStringLiteral("HDR"));
    ASSERT_NE(hdr, nullptr);
    m_control.setState(DisplaySettings{}, false, false);
    EXPECT_FALSE(hdr->isVisible());

    m_control.setState(DisplaySettings{.mode = RangeMode::Hdr}, false, true);
    EXPECT_TRUE(hdr->isVisible());
    EXPECT_TRUE(hdr->isChecked());
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: HDR ▾"));

    // A frame without an HDR tag in a channel set to HDR is shown as Auto, and the control says so.
    m_control.setFrameKind(false, false);
    EXPECT_FALSE(hdr->isVisible());
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: Auto ▾"));
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Auto"));
    EXPECT_TRUE(m_emitted.empty());
}

TEST_F(DisplaySettingsControlTest, ChoosingHdrEmits) {
    m_control.setState(DisplaySettings{.colours = ColourMap::Turbo}, true, true);

    actionTitled(m_control.rangeMenu(), QStringLiteral("HDR"))->trigger();

    ASSERT_EQ(m_emitted.size(), 1u);
    EXPECT_EQ(m_emitted.back(), (DisplaySettings{.mode = RangeMode::Hdr, .colours = ColourMap::Turbo}));
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: HDR ▾"));
}

// From HDR, Hold and Fixed… start from what Auto would show, as they do from Auto.
TEST_F(DisplaySettingsControlTest, LeavingHdrStartsFromTheAutoRange) {
    m_control.setState(DisplaySettings{.mode = RangeMode::Hdr}, true, true);

    openFixed();
    EXPECT_EQ(field("fixedMinimum")->text(), QStringLiteral("10"));
    EXPECT_EQ(field("fixedMaximum")->text(), QStringLiteral("20"));
    popup()->hide();
    actionTitled(m_control.rangeMenu(), QStringLiteral("Hold"))->trigger();

    ASSERT_EQ(m_emitted.size(), 1u);
    EXPECT_EQ(m_emitted.back(), (DisplaySettings{.mode = RangeMode::Hold, .range = {10, 20}}));
}

// New frames change what Auto would show, but never what is being typed.
TEST_F(DisplaySettingsControlTest, FixedFieldsSurviveNewFrames) {
    openFixed();
    type("fixedMinimum", QStringLiteral("5"));

    m_autoRange = {0, 200};
    QTest::qWait(10);

    EXPECT_EQ(field("fixedMinimum")->text(), QStringLiteral("5"));
    EXPECT_EQ(field("fixedMaximum")->text(), QStringLiteral("20"));
    EXPECT_TRUE(popup()->isVisible());
}

// --- Settings applied from elsewhere: the histogram's drags ----------------------------------------------

TEST_F(DisplaySettingsControlTest, ApplySettingsShowsThemAndEmitsOnce) {
    m_control.applySettings({.mode = RangeMode::Fixed, .range = {1, 2}, .colours = ColourMap::Turbo});

    ASSERT_EQ(m_emitted.size(), 1u);
    EXPECT_EQ(m_emitted.back(),
              (DisplaySettings{.mode = RangeMode::Fixed, .range = {1, 2}, .colours = ColourMap::Turbo}));
    EXPECT_EQ(m_control.settings(), m_emitted.back());
    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: Fixed ▾"));
    EXPECT_EQ(button("rangeButton")->toolTip(), QStringLiteral("1 to 2"));
    EXPECT_EQ(checkedTitle(m_control.rangeMenu()), QStringLiteral("Fixed…"));
    EXPECT_EQ(checkedTitle(m_control.coloursMenu()), QStringLiteral("Turbo"));
}

TEST_F(DisplaySettingsControlTest, HdrBlackAndWhiteAreNamedAndChoosingHdrClearsThem) {
    m_control.setState({.mode = RangeMode::Hdr}, false, true);
    m_control.applySettings({.mode = RangeMode::Hdr, .range = {100, 900}});

    EXPECT_EQ(button("rangeButton")->text(), QStringLiteral("Range: HDR ▾"));
    EXPECT_EQ(button("rangeButton")->toolTip(),
              QStringLiteral("HLG or PQ tone-mapped for this screen, black at 100 and white at 900"));

    actionTitled(m_control.rangeMenu(), QStringLiteral("HDR"))->trigger();
    EXPECT_EQ(m_emitted.back(), (DisplaySettings{.mode = RangeMode::Hdr}));
    EXPECT_EQ(button("rangeButton")->toolTip(), QStringLiteral("HLG or PQ tone-mapped for this screen"));
}

TEST_F(DisplaySettingsControlTest, FixedStartsFromHdrsBlackAndWhiteAndMakesFixed) {
    m_control.setState({.mode = RangeMode::Hdr}, false, true);
    m_control.applySettings({.mode = RangeMode::Hdr, .range = {100, 900}});
    openFixed();
    EXPECT_EQ(field("fixedMinimum")->text(), QStringLiteral("100"));
    EXPECT_EQ(field("fixedMaximum")->text(), QStringLiteral("900"));

    applyButton()->click();

    EXPECT_EQ(m_emitted.back(), (DisplaySettings{.mode = RangeMode::Fixed, .range = {100, 900}}));
}

} // namespace
