#include "image_sidebar.h"
#include "live_channel_panel.h"
#include "shades.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRectF>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QToolButton>
#include <QWidget>

#include <gtest/gtest.h>

#include <memory>
#include <optional>

namespace {

QImage frame() {
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    return image;
}

// The panel inside a shown window, as in the sidebar.
struct ShownPanel {
    ShownPanel() : panel(new LiveChannelPanel(&window)) {
        window.resize(220, 600);
        window.show();
    }

    QWidget window;
    LiveChannelPanel* panel;
};

QWidget* rowOf(const LiveChannelPanel& panel, const QString& name) {
    return panel.findChild<QWidget*>(QStringLiteral("liveRow_") + name);
}

QString textIn(const QWidget* row, const char* objectName) {
    const auto* label = row->findChild<QLabel*>(QString::fromLatin1(objectName));
    return label != nullptr ? label->text() : QStringLiteral("<none>");
}

QToolButton* closeButtonOf(const QWidget* row) {
    return row->findChild<QToolButton*>(QStringLiteral("liveClose"));
}

QToolButton* muteButtonOf(const QWidget* row) {
    return row->findChild<QToolButton*>(QStringLiteral("liveMute"));
}

QStringList entryTexts(const QMenu& menu) {
    QStringList texts;
    for (const QAction* action : menu.actions()) {
        if (!action->isSeparator()) {
            texts << action->text();
        }
    }
    return texts;
}

QAction* entryOf(const QMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

QColor dotColourOf(const QWidget* row) {
    auto* dot = row->findChild<QWidget*>(QStringLiteral("liveDot"));
    const QImage image = dot->grab().toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

TEST(LiveChannelPanel, HiddenUntilAChannelStarts) {
    ShownPanel shown;
    EXPECT_TRUE(shown.panel->isHidden());

    shown.panel->addChannel(QStringLiteral("camera"));

    EXPECT_TRUE(shown.panel->isVisible());
    EXPECT_EQ(shown.panel->channelNames(), QStringList{QStringLiteral("camera")});
}

TEST(LiveChannelPanel, RowsKeepStartOrder) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("edges"));
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->addChannel(QStringLiteral("mask"));
    shown.panel->addChannel(QStringLiteral("camera")); // already there
    shown.panel->updateChannel(QStringLiteral("mask"), frame(), 30, {});
    QApplication::processEvents();

    EXPECT_EQ(shown.panel->channelNames(),
              (QStringList{QStringLiteral("edges"), QStringLiteral("camera"), QStringLiteral("mask")}));
    EXPECT_LT(rowOf(*shown.panel, QStringLiteral("edges"))->y(),
              rowOf(*shown.panel, QStringLiteral("camera"))->y());
    EXPECT_LT(rowOf(*shown.panel, QStringLiteral("camera"))->y(),
              rowOf(*shown.panel, QStringLiteral("mask"))->y());
}

TEST(LiveChannelPanel, ShowsRateAndState) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    const QWidget* row = rowOf(*shown.panel, QStringLiteral("camera"));
    ASSERT_NE(row, nullptr);

    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {});
    EXPECT_EQ(textIn(row, "liveRate"), QStringLiteral("30 fps"));
    EXPECT_EQ(textIn(row, "liveState"), QStringLiteral("live"));
    EXPECT_EQ(dotColourOf(row), qt_common::accentColour(QGuiApplication::palette()));
    const auto* thumbnail = row->findChild<QLabel*>(QStringLiteral("liveThumbnail"));
    ASSERT_NE(thumbnail, nullptr);
    EXPECT_FALSE(thumbnail->pixmap().isNull());
    EXPECT_LE(thumbnail->height(), LiveChannelPanel::kRowHeight);

    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 0, {.idle = true});
    EXPECT_EQ(textIn(row, "liveRate"), QStringLiteral("0 fps"));
    EXPECT_EQ(textIn(row, "liveState"), QStringLiteral("idle"));
    EXPECT_NE(dotColourOf(row), qt_common::accentColour(QGuiApplication::palette()));

    // Frozen says so; the dot still shows whether frames arrive, and the word whether they stopped.
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 25, {.frozen = true});
    EXPECT_EQ(textIn(row, "liveState"), QStringLiteral("frozen"));
    EXPECT_EQ(dotColourOf(row), qt_common::accentColour(QGuiApplication::palette()));
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 0, {.idle = true, .frozen = true});
    EXPECT_EQ(textIn(row, "liveState"), QStringLiteral("frozen · idle"));
    EXPECT_NE(dotColourOf(row), qt_common::accentColour(QGuiApplication::palette()));
}

TEST(LiveChannelPanel, StateTexts) {
    EXPECT_EQ(liveStateText({}), QStringLiteral("live"));
    EXPECT_EQ(liveStateText({.idle = true}), QStringLiteral("idle"));
    EXPECT_EQ(liveStateText({.frozen = true}), QStringLiteral("frozen"));
    EXPECT_EQ(liveStateText({.idle = true, .frozen = true}), QStringLiteral("frozen · idle"));
    EXPECT_EQ(liveStateText({.muted = true}), QStringLiteral("muted"));
    EXPECT_EQ(liveStateText({.idle = true, .muted = true}), QStringLiteral("muted · idle"));
}

// (F3) The ✕ is there whenever no frames arrive, frozen or not: a frozen channel whose sender stopped can be
// closed too.
TEST(LiveChannelPanel, CloseShowsWheneverIdle) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    const QWidget* row = rowOf(*shown.panel, QStringLiteral("camera"));
    EXPECT_TRUE(closeButtonOf(row)->isHidden());

    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {});
    EXPECT_TRUE(closeButtonOf(row)->isHidden());
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {.frozen = true});
    EXPECT_TRUE(closeButtonOf(row)->isHidden());
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 0, {.idle = true});
    EXPECT_FALSE(closeButtonOf(row)->isHidden());
    EXPECT_FALSE(closeButtonOf(row)->toolTip().isEmpty());
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 0, {.idle = true, .frozen = true});
    EXPECT_FALSE(closeButtonOf(row)->isHidden());
}

// (R4) A name with a line break stays on the row's one line, as in the info strip and the log.
TEST(LiveChannelPanel, TitleIsOneLine) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("cam\nera"));
    const QWidget* row = rowOf(*shown.panel, QStringLiteral("cam\nera"));
    ASSERT_NE(row, nullptr);

    EXPECT_FALSE(textIn(row, "liveName").contains(QLatin1Char('\n')));
}

TEST(LiveChannelPanel, ClickSelectsAndEmits) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->addChannel(QStringLiteral("edges"));
    QApplication::processEvents();
    QSignalSpy selected(shown.panel, &LiveChannelPanel::channelSelected);

    QTest::mouseClick(rowOf(*shown.panel, QStringLiteral("edges")), Qt::LeftButton);

    ASSERT_EQ(selected.size(), 1);
    EXPECT_EQ(selected.first().at(0).toString(), QStringLiteral("edges"));
    EXPECT_EQ(shown.panel->selectedChannel(), QStringLiteral("edges"));
}

// (F7) A muted channel cannot be looked at, so its row does not light up as chosen when clicked.
TEST(LiveChannelPanel, ClickOnAMutedRowDoesNothing) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {.muted = true});
    QApplication::processEvents();
    QSignalSpy selected(shown.panel, &LiveChannelPanel::channelSelected);

    QTest::mouseClick(rowOf(*shown.panel, QStringLiteral("camera")), Qt::LeftButton);

    EXPECT_TRUE(selected.isEmpty());
    EXPECT_EQ(shown.panel->selectedChannel(), std::nullopt);
}

// Choosing a channel leaves the keyboard where it was, so the arrow keys still step through the thumbnails.
TEST(LiveChannelPanel, ClickLeavesTheFocusWhereItWas) {
    QWidget window;
    auto* field = new QLineEdit(&window);
    auto* panel = new LiveChannelPanel(&window);
    panel->move(0, 40);
    window.resize(220, 300);
    window.show();
    window.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window));
    panel->addChannel(QStringLiteral("camera"));
    QApplication::processEvents();
    field->setFocus();

    QTest::mouseClick(rowOf(*panel, QStringLiteral("camera")), Qt::LeftButton);

    EXPECT_EQ(panel->selectedChannel(), QStringLiteral("camera"));
    EXPECT_TRUE(field->hasFocus());
}

TEST(LiveChannelPanel, SetSelectedIsQuiet) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    QSignalSpy selected(shown.panel, &LiveChannelPanel::channelSelected);

    shown.panel->setSelected(QStringLiteral("camera"));
    EXPECT_EQ(shown.panel->selectedChannel(), QStringLiteral("camera"));
    shown.panel->setSelected(std::nullopt);
    EXPECT_EQ(shown.panel->selectedChannel(), std::nullopt);
    shown.panel->setSelected(QStringLiteral("nobody"));
    EXPECT_EQ(shown.panel->selectedChannel(), std::nullopt);

    EXPECT_TRUE(selected.isEmpty());
}

TEST(LiveChannelPanel, CloseButtonEmits) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 0, {.idle = true});
    QSignalSpy closeRequested(shown.panel, &LiveChannelPanel::closeRequested);
    QSignalSpy selected(shown.panel, &LiveChannelPanel::channelSelected);

    closeButtonOf(rowOf(*shown.panel, QStringLiteral("camera")))->click();

    ASSERT_EQ(closeRequested.size(), 1);
    EXPECT_EQ(closeRequested.first().at(0).toString(), QStringLiteral("camera"));
    EXPECT_TRUE(selected.isEmpty());
    EXPECT_EQ(shown.panel->channelNames(), QStringList{QStringLiteral("camera")}); // the window decides
}

// (F7) The row's button and its right-click menu ask the window to mute; the window decides, and the row
// shows what it decided.
TEST(LiveChannelPanel, MuteButtonAndMenuRequestMute) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {});
    shown.panel->setSelected(QStringLiteral("camera"));
    QWidget* row = rowOf(*shown.panel, QStringLiteral("camera"));
    QSignalSpy muteRequested(shown.panel, &LiveChannelPanel::muteRequested);
    QSignalSpy closeRequested(shown.panel, &LiveChannelPanel::closeRequested);
    EXPECT_EQ(muteButtonOf(row)->toolTip(), QStringLiteral("Mute this channel"));

    muteButtonOf(row)->click();

    ASSERT_EQ(muteRequested.size(), 1);
    EXPECT_EQ(muteRequested.first().at(0).toString(), QStringLiteral("camera"));
    EXPECT_TRUE(muteRequested.first().at(1).toBool());
    EXPECT_FALSE(muteButtonOf(row)->isChecked()); // not until the window says so

    const std::unique_ptr<QMenu> live = shown.panel->createContextMenu(QStringLiteral("camera"));
    EXPECT_EQ(entryTexts(*live), (QStringList{QStringLiteral("Mute"), QStringLiteral("Close")}));
    EXPECT_FALSE(entryOf(*live, QStringLiteral("Close"))->isEnabled()); // it would start again at once

    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 0, {.idle = true, .muted = true});
    EXPECT_TRUE(muteButtonOf(row)->isChecked());
    EXPECT_EQ(muteButtonOf(row)->toolTip(), QStringLiteral("Unmute this channel"));
    const std::unique_ptr<QMenu> muted = shown.panel->createContextMenu(QStringLiteral("camera"));
    EXPECT_EQ(entryTexts(*muted), (QStringList{QStringLiteral("Unmute"), QStringLiteral("Close")}));
    entryOf(*muted, QStringLiteral("Unmute"))->trigger();
    ASSERT_EQ(muteRequested.size(), 2);
    EXPECT_FALSE(muteRequested.last().at(1).toBool());
    entryOf(*muted, QStringLiteral("Close"))->trigger();
    ASSERT_EQ(closeRequested.size(), 1);
    EXPECT_EQ(closeRequested.first().at(0).toString(), QStringLiteral("camera"));
}

// (F7, owner's choice) Like the thumbnails' icons: on the row under the pointer and the row in view; always
// on a muted row, so its state stays in sight.
TEST(LiveChannelPanel, MuteButtonShowsOnHoverSelectionOrMute) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {});
    QWidget* row = rowOf(*shown.panel, QStringLiteral("camera"));
    EXPECT_TRUE(muteButtonOf(row)->isHidden());

    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), row->mapToGlobal(QPointF(5, 5)));
    QApplication::sendEvent(row, &enter);
    EXPECT_FALSE(muteButtonOf(row)->isHidden());
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(row, &leave);
    EXPECT_TRUE(muteButtonOf(row)->isHidden());

    shown.panel->setSelected(QStringLiteral("camera"));
    EXPECT_FALSE(muteButtonOf(row)->isHidden());
    shown.panel->setSelected(std::nullopt);
    EXPECT_TRUE(muteButtonOf(row)->isHidden());

    shown.panel->updateChannel(QStringLiteral("camera"), frame(), 30, {.muted = true});
    EXPECT_FALSE(muteButtonOf(row)->isHidden());
    EXPECT_TRUE(muteButtonOf(row)->isChecked());
}

TEST(LiveChannelPanel, HidesWhenTheLastRowGoes) {
    ShownPanel shown;
    shown.panel->addChannel(QStringLiteral("camera"));
    shown.panel->addChannel(QStringLiteral("edges"));
    shown.panel->setSelected(QStringLiteral("camera"));

    shown.panel->removeChannel(QStringLiteral("camera"));
    EXPECT_TRUE(shown.panel->isVisible());
    EXPECT_EQ(shown.panel->selectedChannel(), std::nullopt);
    EXPECT_EQ(rowOf(*shown.panel, QStringLiteral("camera")), nullptr);

    shown.panel->removeChannel(QStringLiteral("edges"));
    EXPECT_TRUE(shown.panel->isHidden());
    EXPECT_TRUE(shown.panel->channelNames().isEmpty());
}

TEST(LiveChannelPanel, ScrollsBeyondThreeRows) {
    ShownPanel shown;
    auto* scroll = shown.panel->findChild<QScrollArea*>();
    ASSERT_NE(scroll, nullptr);

    shown.panel->addChannel(QStringLiteral("a"));
    shown.panel->addChannel(QStringLiteral("b"));
    QApplication::processEvents();
    EXPECT_EQ(scroll->viewport()->height(), 2 * LiveChannelPanel::kRowHeight);
    EXPECT_EQ(scroll->verticalScrollBar()->maximum(), 0);

    shown.panel->addChannel(QStringLiteral("c"));
    shown.panel->addChannel(QStringLiteral("d"));
    // The scroll range follows the rows' layout through posted events, which the running viewer's loop
    // delivers.
    EXPECT_TRUE(QTest::qWaitFor([scroll] { return scroll->verticalScrollBar()->maximum() > 0; }));
    EXPECT_EQ(scroll->viewport()->height(), LiveChannelPanel::kVisibleRows * LiveChannelPanel::kRowHeight);
    EXPECT_EQ(scroll->verticalScrollBar()->maximum(), LiveChannelPanel::kRowHeight);
}

// Sends a left-button event to `widget` at `local`, with the global position the drag has reached.
void sendGripEvent(QWidget& widget, QEvent::Type type, QPointF local, QPointF global) {
    QMouseEvent event(type, local, global, Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

// Drags the grip below the rows down by `dy` (up when negative), as a person would: it moves with the panel.
void dragGrip(LiveChannelPanel& panel, int dy) {
    auto* grip = panel.findChild<QWidget*>(QStringLiteral("liveRowsGrip"));
    ASSERT_NE(grip, nullptr);
    const QPointF centre = QRectF(grip->rect()).center();
    const QPointF start = grip->mapToGlobal(centre);
    sendGripEvent(*grip, QEvent::MouseButtonPress, centre, start);
    sendGripEvent(*grip, QEvent::MouseMove, centre + QPointF(0, dy), start + QPointF(0, dy));
    sendGripEvent(*grip, QEvent::MouseButtonRelease, centre + QPointF(0, dy), start + QPointF(0, dy));
}

void addChannels(LiveChannelPanel& panel, int count) {
    for (int i = 0; i < count; ++i) {
        panel.addChannel(QStringLiteral("camera %1").arg(i));
    }
}

int shownRowsHeight(const LiveChannelPanel& panel) {
    return panel.findChild<QScrollArea*>()->viewport()->height();
}

// (Owner, walkthrough 2026-11-01) The grip below the rows shows more or fewer of them, in whole rows: from
// one up to every channel the viewer takes.
TEST(LiveChannelPanel, DraggingTheGripShowsMoreOrFewerRows) {
    ShownPanel shown;
    addChannels(*shown.panel, 8);
    QApplication::processEvents();
    ASSERT_EQ(shownRowsHeight(*shown.panel), LiveChannelPanel::kVisibleRows * LiveChannelPanel::kRowHeight);

    dragGrip(*shown.panel, 2 * LiveChannelPanel::kRowHeight + 5);
    QApplication::processEvents();
    EXPECT_EQ(shown.panel->visibleRows(), 5);
    EXPECT_EQ(shownRowsHeight(*shown.panel), 5 * LiveChannelPanel::kRowHeight);

    dragGrip(*shown.panel, 20 * LiveChannelPanel::kRowHeight);
    EXPECT_EQ(shown.panel->visibleRows(), LiveChannelPanel::kMaxVisibleRows);

    dragGrip(*shown.panel, -20 * LiveChannelPanel::kRowHeight);
    QApplication::processEvents();
    EXPECT_EQ(shown.panel->visibleRows(), 1);
    EXPECT_EQ(shownRowsHeight(*shown.panel), LiveChannelPanel::kRowHeight);
}

// Fewer channels than the rows chosen take only their own rows; the choice is kept for when more come.
TEST(LiveChannelPanel, FewerChannelsThanRowsFitThemselves) {
    ShownPanel shown;
    shown.panel->setVisibleRows(6);
    addChannels(*shown.panel, 2);
    QApplication::processEvents();

    EXPECT_EQ(shownRowsHeight(*shown.panel), 2 * LiveChannelPanel::kRowHeight);
    EXPECT_EQ(shown.panel->visibleRows(), 6);
}

TEST(LiveChannelPanel, VisibleRowsAreRemembered) {
    {
        LiveChannelPanel panel;
        EXPECT_EQ(panel.visibleRows(), LiveChannelPanel::kVisibleRows);
        panel.setVisibleRows(6);
    }
    EXPECT_EQ(LiveChannelPanel().visibleRows(), 6);

    LiveChannelPanel panel;
    panel.setVisibleRows(0);
    EXPECT_EQ(panel.visibleRows(), 1);
    panel.setVisibleRows(100);
    EXPECT_EQ(panel.visibleRows(), LiveChannelPanel::kMaxVisibleRows);
}

TEST(LiveChannelPanel, SidebarShowsItAboveTheSearch) {
    ImageSidebar sidebar;
    sidebar.resize(ImageSidebar::kDefaultWidth, 600);
    sidebar.show();
    LiveChannelPanel* panel = sidebar.liveChannels();
    ASSERT_NE(panel, nullptr);
    EXPECT_TRUE(panel->isHidden());

    panel->addChannel(QStringLiteral("camera"));
    QApplication::processEvents();

    EXPECT_TRUE(panel->isVisible());
    EXPECT_EQ(panel->width(), sidebar.width());
    EXPECT_LE(panel->geometry().bottom(), sidebar.searchField()->mapTo(&sidebar, QPoint(0, 0)).y());
}

} // namespace
