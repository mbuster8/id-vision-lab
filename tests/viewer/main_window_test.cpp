#include "window_test_support.h"

#include "activity_log.h"
#include "appearance.h"
#include "control_checks.h"
#include "data_folder.h"
#include "display_settings_control.h"
#include "display_text.h"
#include "epoch_time.h"
#include "histogram_strip.h"
#include "image_info_bar.h"
#include "image_sidebar.h"
#include "image_store.h"
#include "image_view.h"
#include "latch.h"
#include "live_channel_panel.h"
#include "live_converter.h"
#include "palette_support.h"
#include "server_controls.h"
#include "shades.h"
#include "snapshot_converter.h"
#include "storage_worker.h"
#include "thumbnail_model.h"
#include "thumbnail_panel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPalette>
#include <QPoint>
#include <QPushButton>
#include <QRectF>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include "viewer_core/image_history.h"
#include "visual_debug/client.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using test_support::makeTestWindow;
using viewer_core::ImageId;

cv::Mat makeImage(int rows = 3, int cols = 4) { return cv::Mat(rows, cols, CV_8UC1, cv::Scalar(9)); }

QAction* action(MainWindow& window, const char* name) {
    return window.findChild<QAction*>(QString::fromLatin1(name));
}

ThumbnailPanel* panelOf(MainWindow& window) { return window.findChild<ThumbnailPanel*>(); }

ImageInfoBar* infoBarOf(MainWindow& window) { return window.findChild<ImageInfoBar*>(); }

ImageSidebar* sidebarOf(MainWindow& window) { return window.findChild<ImageSidebar*>(); }

ServerControls* serverControlsOf(MainWindow& window) { return window.findChild<ServerControls*>(); }

ActivityLog* logOf(MainWindow& window) { return window.findChild<ActivityLog*>(); }

// The newest entry whose text contains `text`, or null. Row 0 is the newest.
const LogEntry* findEntry(const ActivityLog& log, const QString& text) {
    for (int row = 0; row < log.rowCount(); ++row) {
        if (log.entryAt(row)->text.contains(text)) {
            return log.entryAt(row);
        }
    }
    return nullptr;
}

int countEntries(const ActivityLog& log, const QString& text) {
    int count = 0;
    for (int row = 0; row < log.rowCount(); ++row) {
        count += log.entryAt(row)->text.contains(text) ? 1 : 0;
    }
    return count;
}

// The test windows listen on an ephemeral port: the endpoint is read from "Listening on [<endpoint>]".
QString listeningEndpoint(MainWindow& window) {
    ServerControls* controls = serverControlsOf(window);
    const bool listening =
        QTest::qWaitFor([controls] { return controls->state() == ServerState::Listening; }, 2000);
    const QString text = controls->text();
    return listening && text.contains(QLatin1Char('['))
               ? text.section(QLatin1Char('['), 1, 1).section(QLatin1Char(']'), 0, 0)
               : QString();
}

visual_debug::SendResult sendTo(const QString& endpoint) {
    visual_debug::Client client(
        {.endpoint = endpoint.toStdString(), .replyTimeout = std::chrono::milliseconds(300)});
    return client.send(makeImage(), "sent");
}

// The name the sidebar lists for the id, or a null string when it is not listed.
QString listedName(MainWindow& window, ImageId id) {
    const QAbstractItemModel* model = panelOf(window)->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(ThumbnailModel::IdRole).value<quint64>() == id) {
            return index.data(ThumbnailModel::NameRole).toString();
        }
    }
    return {};
}

// Adds a, b, c (ids 1, 2, 3); the sidebar lists c, b, a.
void addThree(MainWindow& window) {
    window.addImage("a", makeImage());
    window.addImage("b", makeImage());
    window.addImage("c", makeImage());
}

// The walkthrough found an empty strip between the thumbnails and the image: the sidebar must fill its
// splitter section exactly, with the image area directly beside it and the image view filling its width.
TEST(MainWindowTest, SidebarFillsItsSplitterSection) {
    const auto window = makeTestWindow();
    window->resize(1000, 700);
    window->show();
    QApplication::processEvents();

    const auto* splitter = window->findChild<QSplitter*>(QStringLiteral("mainSplitter"));
    const auto* sidebar = window->findChild<ImageSidebar*>();
    const auto* panel = window->findChild<ThumbnailPanel*>();
    const auto* view = window->findChild<ImageView*>();
    ASSERT_NE(splitter, nullptr);
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(view, nullptr);

    EXPECT_EQ(splitter->widget(0), sidebar);
    EXPECT_EQ(sidebar->width(), splitter->sizes()[0]);
    EXPECT_EQ(panel->width(), sidebar->width());
    EXPECT_TRUE(splitter->widget(1)->isAncestorOf(view));
    EXPECT_EQ(view->width(), splitter->widget(1)->width());
}

TEST(MainWindowTest, WindowTitleNamesImageAndEndpoint) {
    EXPECT_EQ(viewerWindowTitle(QStringLiteral("Sobel X"), QStringLiteral("tcp://127.0.0.1:7139")),
              QString::fromUtf8("Visual Debug Viewer — Sobel X — 127.0.0.1:7139"));
    EXPECT_EQ(viewerWindowTitle(QString(), QStringLiteral("tcp://127.0.0.1:7140")),
              QString::fromUtf8("Visual Debug Viewer — 127.0.0.1:7140"));
}

TEST(MainWindowTest, TitleShowsTheEndpointBeforeAnyImage) {
    const auto window = makeTestWindow(QStringLiteral("tcp://127.0.0.1:7141"));

    EXPECT_EQ(window->windowTitle(), QString::fromUtf8("Visual Debug Viewer — 127.0.0.1:7141"));
}

TEST(MainWindowTest, HelpMenuOffersAbout) {
    const auto window = makeTestWindow(QStringLiteral("tcp://127.0.0.1:7141"));

    const auto* help = window->findChild<QMenu*>(QStringLiteral("menuHelp"));
    ASSERT_NE(help, nullptr);
    ASSERT_FALSE(help->actions().isEmpty());
    EXPECT_EQ(help->actions().front()->menuRole(), QAction::AboutRole);
}

TEST(MainWindowTest, PixelReadoutHasALeftMargin) {
    const auto window = makeTestWindow();

    const auto* readout = window->findChild<QLabel*>(QStringLiteral("pixelReadout"));

    ASSERT_NE(readout, nullptr);
    EXPECT_GT(readout->contentsMargins().left(), 0);
}

TEST(MainWindowTest, DeleteSelectsNextOlder) {
    const auto window = makeTestWindow();
    addThree(*window);
    ASSERT_TRUE(panelOf(*window)->selectImage(2));

    action(*window, "actionDelete")->trigger();

    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(*window)->listedIds(), (std::vector<ImageId>{3, 1}));
}

TEST(MainWindowTest, DeleteOfTheOldestSelectsTheNextNewer) {
    const auto window = makeTestWindow();
    addThree(*window);
    ASSERT_TRUE(panelOf(*window)->selectImage(1));

    action(*window, "actionDelete")->trigger();

    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
}

TEST(MainWindowTest, UndoDeleteRestoresAtItsPlace) {
    const auto window = makeTestWindow();
    addThree(*window);
    ASSERT_TRUE(panelOf(*window)->selectImage(2));
    action(*window, "actionDelete")->trigger();

    action(*window, "actionUndoDelete")->trigger();

    EXPECT_EQ(panelOf(*window)->listedIds(), (std::vector<ImageId>{3, 2, 1}));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(2));
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
}

TEST(MainWindowTest, DeletingTheLastImageClearsEverything) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    auto* readout = window->findChild<QLabel*>(QStringLiteral("pixelReadout"));
    readout->setText(QStringLiteral("X=0 Y=0 · I=9"));

    action(*window, "actionDelete")->trigger();

    EXPECT_TRUE(infoBarOf(*window)->isHidden());
    EXPECT_EQ(window->windowTitle(), viewerWindowTitle(QString(), QStringLiteral("tcp://127.0.0.1:*")));
    EXPECT_TRUE(readout->text().isEmpty());
    EXPECT_FALSE(window->findChild<ImageView*>()->hasImage());
    EXPECT_EQ(window->shownImageId(), std::nullopt);
    EXPECT_TRUE(panelOf(*window)->listedIds().empty());
}

TEST(MainWindowTest, DuplicateAddsCopyAboveAndSelectsIt) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());

    action(*window, "actionDuplicate")->trigger();

    const std::vector<ImageId> listed = panelOf(*window)->listedIds();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listedName(*window, listed[0]), QStringLiteral("a copy"));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(listed[0]));
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(listed[0]));
}

TEST(MainWindowTest, CopyImagePutsDisplayOnClipboard) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage(6, 8));

    action(*window, "actionCopyImage")->trigger();

    EXPECT_EQ(QGuiApplication::clipboard()->image().size(), QSize(8, 6));
}

TEST(MainWindowTest, RenameUpdatesCaptionTitleAndInfo) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());

    window->renameImage(1, QStringLiteral("  new  "));

    EXPECT_EQ(listedName(*window, 1), QStringLiteral("new"));
    EXPECT_TRUE(window->windowTitle().contains(QStringLiteral("new")));
    EXPECT_EQ(infoBarOf(*window)->findChild<QLabel*>(QStringLiteral("imageName"))->text(),
              QStringLiteral("new"));
}

TEST(MainWindowTest, RenameIgnoresEmptyName) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());

    window->renameImage(1, QStringLiteral("   "));

    EXPECT_EQ(listedName(*window, 1), QStringLiteral("a"));
}

TEST(MainWindowTest, RenameCutsLongNameAtCharacterBoundary) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());

    window->renameImage(1, QString(1023, u'a') + QString::fromUtf8("é"));

    EXPECT_EQ(listedName(*window, 1), QString(1023, u'a'));
}

// The wire format's limit, so that a stored copy reloads with its " copy" instead of its original's name.
TEST(MainWindowTest, DuplicateOfLongNameStaysWithinTheNameLimit) {
    const auto window = makeTestWindow();
    window->addImage(std::string(1024, 'a'), makeImage());

    window->duplicateImage(1);

    EXPECT_EQ(listedName(*window, 2), QString(1019, u'a') + QStringLiteral(" copy"));
}

TEST(MainWindowTest, ArrivalsDuringRenameDoNotMoveTheEdit) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    action(*window, "actionRename")->trigger();

    window->addImage("b", makeImage());
    window->addImage("c", makeImage());
    window->addImage("d", makeImage());

    EXPECT_EQ(infoBarOf(*window)->renamingId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(1));
    auto* editor = window->findChild<QLineEdit*>(QStringLiteral("renameEditor"));
    ASSERT_NE(editor, nullptr);
    editor->setText(QStringLiteral("x"));
    QTest::keyClick(editor, Qt::Key_Return);
    EXPECT_EQ(listedName(*window, 1), QStringLiteral("x"));
    EXPECT_EQ(listedName(*window, 4), QStringLiteral("d"));
}

// The keyboard starts on the thumbnails: the arrow keys step through the images, and the search field shows
// no focus ring in the accent colour until it is used (spec H2).
TEST(MainWindowTest, OpensWithTheThumbnailsFocused) {
    const auto window = makeTestWindow();
    window->show();
    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));

    EXPECT_EQ(QApplication::focusWidget(), panelOf(*window));
}

// Without a click first, Down steps to the next older image and shows it.
TEST(MainWindowTest, ArrowKeysStepThroughTheImagesFromTheStart) {
    const auto window = makeTestWindow();
    window->show();
    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));
    addThree(*window);
    ASSERT_TRUE(panelOf(*window)->selectImage(3));

    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);

    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
}

TEST(MainWindowTest, EditActionsFollowFocus) {
    const auto window = makeTestWindow();
    window->show();
    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));
    window->addImage("a", makeImage(6, 8));
    panelOf(*window)->setFocus();
    QGuiApplication::clipboard()->clear();

    // From the sidebar, the shortcut copies the image.
    QTest::keyClick(panelOf(*window), Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QGuiApplication::clipboard()->image().size(), QSize(8, 6));

    // From a text field, the shortcuts act on its text.
    QGuiApplication::clipboard()->clear();
    action(*window, "actionRename")->trigger();
    auto* editor = window->findChild<QLineEdit*>(QStringLiteral("renameEditor"));
    ASSERT_NE(editor, nullptr);
    ASSERT_TRUE(editor->hasFocus());
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Backspace, Qt::ControlModifier);
    EXPECT_NE(QGuiApplication::clipboard()->image().size(), QSize(8, 6));
    EXPECT_EQ(panelOf(*window)->listedIds().size(), 1u);
}

// The log panel takes ⌘C, ⌘Z and ⌘⌫ as a text field does: they never reach the image.
TEST(MainWindowTest, LogPanelKeepsTheEditKeys) {
    const auto window = makeTestWindow();
    window->show();
    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));
    window->addImage("kept", makeImage(6, 8));
    window->addImage("deleted", makeImage(6, 8));
    panelOf(*window)->setFocus();
    action(*window, "actionDelete")->trigger();
    ASSERT_EQ(panelOf(*window)->listedIds().size(), 1u);
    window->findChild<QToolButton*>(QStringLiteral("logToggle"))->click();
    auto* logList = window->findChild<QListView*>(QStringLiteral("logList"));
    ASSERT_NE(logList, nullptr);
    logList->setFocus();
    ASSERT_TRUE(logList->hasFocus());
    EXPECT_FALSE(action(*window, "actionUndoDelete")->isEnabled());
    QGuiApplication::clipboard()->clear();

    // With nothing selected, every entry, as the panel's Copy button does.
    QTest::keyClick(logList, Qt::Key_C, Qt::ControlModifier);
    EXPECT_TRUE(QGuiApplication::clipboard()->image().isNull());
    EXPECT_EQ(QGuiApplication::clipboard()->text(), logOf(*window)->toPlainText({}));

    QTest::keyClick(logList, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(panelOf(*window)->listedIds().size(), 1u);
    QTest::keyClick(logList, Qt::Key_Backspace, Qt::ControlModifier);
    EXPECT_EQ(panelOf(*window)->listedIds().size(), 1u);
}

TEST(MainWindowTest, EvictingTheRenamedImageCancelsTheRename) {
    const auto window = makeTestWindow();
    window->addImage("renamed", makeImage());
    action(*window, "actionRename")->trigger();

    for (int i = 0; i < 100; ++i) {
        window->addImage("filler", makeImage());
    }

    EXPECT_FALSE(infoBarOf(*window)->isRenaming());
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2)); // directly above it, now the oldest
}

TEST(MainWindowTest, HiddenArrivalsDoNotTakeTheView) {
    const auto window = makeTestWindow();
    sidebarOf(*window)->searchField()->setText(QStringLiteral("canny"));

    window->addImage("Canny 1", makeImage());
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(1));

    window->addImage("Sobel", makeImage());
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(sidebarOf(*window)->countLabel()->text(), QStringLiteral("1 of 2"));

    window->addImage("Canny 2", makeImage());
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(3));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(3));
}

TEST(MainWindowTest, SearchKeepsTheShownImage) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage(3, 4));
    window->addImage("b", makeImage(6, 8));
    ASSERT_TRUE(panelOf(*window)->selectImage(2));

    sidebarOf(*window)->searchField()->setText(QStringLiteral("a"));

    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
    EXPECT_EQ(infoBarOf(*window)->findChild<QLabel*>(QStringLiteral("imageName"))->text(),
              QStringLiteral("b"));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::nullopt);
    action(*window, "actionCopyImage")->trigger();
    EXPECT_EQ(QGuiApplication::clipboard()->image().size(), QSize(8, 6));
}

TEST(MainWindowTest, DeleteUnderSortSelectsNextListed) {
    const auto window = makeTestWindow();
    window->addImage("c", makeImage());
    window->addImage("a", makeImage());
    window->addImage("b", makeImage());
    sidebarOf(*window)->sortAction(SidebarSort::Name)->trigger(); // a, b, c
    ASSERT_TRUE(panelOf(*window)->selectImage(3));

    action(*window, "actionDelete")->trigger();
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1)); // c; by history order it would be a
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(1));

    action(*window, "actionDelete")->trigger();
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
}

TEST(MainWindowTest, DeletingAnUnlistedImageShowsFirstListed) {
    const auto window = makeTestWindow();
    window->addImage("x1", makeImage());
    window->addImage("y", makeImage());
    window->addImage("x2", makeImage());
    ASSERT_TRUE(panelOf(*window)->selectImage(2));
    sidebarOf(*window)->searchField()->setText(QStringLiteral("x"));

    action(*window, "actionDelete")->trigger();
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(3));

    sidebarOf(*window)->searchField()->setText(QStringLiteral("zzz"));
    action(*window, "actionDelete")->trigger();
    EXPECT_EQ(window->shownImageId(), std::nullopt);
    EXPECT_FALSE(window->findChild<ImageView*>()->hasImage());
}

TEST(MainWindowTest, UndoUnderSearchShowsRestoredImage) {
    const auto window = makeTestWindow();
    window->addImage("a1", makeImage());
    window->addImage("a2", makeImage());
    window->addImage("b1", makeImage());
    QLineEdit* search = sidebarOf(*window)->searchField();
    search->setText(QStringLiteral("a"));
    ASSERT_TRUE(panelOf(*window)->selectImage(2));
    action(*window, "actionDelete")->trigger();

    search->setText(QStringLiteral("b"));
    action(*window, "actionUndoDelete")->trigger();

    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::nullopt);
    EXPECT_EQ(search->text(), QStringLiteral("b"));
}

TEST(MainWindowTest, FindFocusesSearch) {
    const auto window = makeTestWindow();
    window->show();
    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));

    action(*window, "actionFind")->trigger();

    EXPECT_TRUE(sidebarOf(*window)->searchField()->hasFocus());
}

TEST(MainWindowTest, StopReleasesThePort) {
    const auto window = makeTestWindow();
    const QString endpoint = listeningEndpoint(*window);
    ASSERT_FALSE(endpoint.isEmpty());

    action(*window, "actionStopServer")->trigger();

    EXPECT_EQ(serverControlsOf(*window)->state(), ServerState::Stopped);
    EXPECT_EQ(serverControlsOf(*window)->text(), QStringLiteral("Stopped: released [%1]").arg(endpoint));
    EXPECT_EQ(action(*window, "actionStopServer")->text(), QStringLiteral("Start"));
    EXPECT_EQ(sendTo(endpoint), visual_debug::SendResult::ViewerUnavailable);
}

// Stopped while paused: a started server takes images again.
TEST(MainWindowTest, StartListensAgain) {
    const auto window = makeTestWindow();
    ASSERT_FALSE(listeningEndpoint(*window).isEmpty());
    action(*window, "actionPauseServer")->trigger();
    action(*window, "actionStopServer")->trigger();

    action(*window, "actionStopServer")->trigger();

    EXPECT_EQ(serverControlsOf(*window)->state(), ServerState::Listening);
    EXPECT_EQ(action(*window, "actionStopServer")->text(), QStringLiteral("Stop"));
    EXPECT_EQ(sendTo(listeningEndpoint(*window)), visual_debug::SendResult::Delivered);
}

TEST(MainWindowTest, PauseRejectsSenders) {
    const auto window = makeTestWindow();
    const QString endpoint = listeningEndpoint(*window);
    ASSERT_FALSE(endpoint.isEmpty());

    action(*window, "actionPauseServer")->trigger();
    EXPECT_EQ(serverControlsOf(*window)->state(), ServerState::Paused);
    EXPECT_EQ(serverControlsOf(*window)->text(), QStringLiteral("Paused: senders get [viewer paused]"));
    EXPECT_EQ(action(*window, "actionPauseServer")->text(), QStringLiteral("Resume"));
    EXPECT_EQ(sendTo(endpoint), visual_debug::SendResult::Rejected);

    action(*window, "actionPauseServer")->trigger();
    EXPECT_EQ(serverControlsOf(*window)->state(), ServerState::Listening);
    EXPECT_EQ(sendTo(endpoint), visual_debug::SendResult::Delivered);
}

// The window holds its endpoint's data folder, so a second viewer on the same endpoint keeps its data in
// memory. The server is never started here: the event loop does not run.
TEST(MainWindowTest, HoldsItsDataFolder) {
    const QTemporaryDir root;
    const QString endpoint = QStringLiteral("tcp://127.0.0.1:47139");
    {
        const auto window = makeTestWindow(endpoint, root.path());

        EXPECT_TRUE(QFileInfo(QDir(root.path()).filePath(QStringLiteral("tcp_127.0.0.1_47139"))).isDir());
        EXPECT_EQ(DataFolder::open(root.path(), endpoint), nullptr);
    }
    EXPECT_NE(DataFolder::open(root.path(), endpoint), nullptr);
}

TEST(MainWindowTest, LogsReceivedImageWithItsReceivedTime) {
    const auto window = makeTestWindow();
    const QString endpoint = listeningEndpoint(*window);
    ASSERT_FALSE(endpoint.isEmpty());
    visual_debug::Client client({.endpoint = endpoint.toStdString()});
    const auto before = std::chrono::system_clock::now();

    ASSERT_EQ(client.send(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)), "camera frame"),
              visual_debug::SendResult::Delivered);
    const auto sent = std::chrono::system_clock::now();
    // The UI takes the image only after this pause: a time taken by the UI would come after `sent`.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const ActivityLog* log = logOf(*window);
    const LogEntry* received = nullptr;
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            received = findEntry(*log, QStringLiteral("camera frame"));
            return received != nullptr;
        },
        2000));

    EXPECT_EQ(received->kind, LogKind::Info);
    EXPECT_TRUE(received->text.contains(QStringLiteral("640×480"))) << received->text.toStdString();
    const QString pixelBytes = QLocale().formattedDataSize(640 * 480 * 3, 1, QLocale::DataSizeSIFormat);
    EXPECT_TRUE(received->text.contains(pixelBytes)) << received->text.toStdString();
    EXPECT_GE(received->time, before);
    EXPECT_LE(received->time, sent);
}

// A sender streaming into a paused viewer is told so on every frame; the log says it once, when it matters.
TEST(MainWindowTest, PausedStreamingLogsOnceAtResume) {
    const auto window = makeTestWindow();
    const QString endpoint = listeningEndpoint(*window);
    ASSERT_FALSE(endpoint.isEmpty());
    action(*window, "actionPauseServer")->trigger();
    const ActivityLog* log = logOf(*window);
    visual_debug::Client client(
        {.endpoint = endpoint.toStdString(), .replyTimeout = std::chrono::milliseconds(300)});

    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(client.sendLive(makeImage(), "camera"), visual_debug::SendResult::Rejected);
    }
    QTest::qWait(50);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Rejected")), 0);

    action(*window, "actionPauseServer")->trigger(); // Resume

    EXPECT_EQ(countEntries(*log, QStringLiteral("Rejected")), 1);
    const LogEntry* rejected = findEntry(*log, QStringLiteral("Rejected"));
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->text, QStringLiteral("Rejected 3 requests while paused: [viewer paused]"));
    EXPECT_EQ(rejected->kind, LogKind::Warning);

    // Stop says it too.
    action(*window, "actionPauseServer")->trigger();
    ASSERT_EQ(client.sendLive(makeImage(), "camera"), visual_debug::SendResult::Rejected);
    action(*window, "actionStopServer")->trigger();
    EXPECT_NE(findEntry(*log, QStringLiteral("Rejected a request while paused: [viewer paused]")), nullptr);
}

// Any other reason: the first count at once, then at most one line a second, however fast they come.
TEST(MainWindowTest, RejectionsAreCoalescedPerSecond) {
    const auto window = makeTestWindow();
    const ActivityLog* log = logOf(*window);
    QtImageSink& sink = window->sinkForTests();

    for (int i = 0; i < 3; ++i) {
        sink.onRequestRejected("viewer busy");
    }
    QApplication::processEvents();
    EXPECT_EQ(countEntries(*log, QStringLiteral("Rejected")), 1);
    EXPECT_EQ(log->latest()->text, QStringLiteral("Rejected 3 requests: [viewer busy]"));

    sink.onRequestRejected("viewer busy");
    sink.onRequestRejected("viewer busy");
    QTest::qWait(100);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Rejected")), 1);

    ASSERT_TRUE(QTest::qWaitFor([log] { return countEntries(*log, QStringLiteral("Rejected")) == 2; }, 3000));
    EXPECT_EQ(log->latest()->text, QStringLiteral("Rejected 2 requests: [viewer busy]"));
}

TEST(MainWindowTest, LogsServerStates) {
    const auto window = makeTestWindow();
    const QString endpoint = listeningEndpoint(*window);
    ASSERT_FALSE(endpoint.isEmpty());
    const ActivityLog* log = logOf(*window);
    EXPECT_NE(findEntry(*log, QStringLiteral("Listening on [%1]").arg(endpoint)), nullptr);

    action(*window, "actionPauseServer")->trigger();
    EXPECT_EQ(log->latest()->text, QStringLiteral("Paused: senders get [viewer paused]"));
    action(*window, "actionStopServer")->trigger();
    EXPECT_EQ(log->latest()->text, QStringLiteral("Stopped: released [%1]").arg(endpoint));
}

// A start that fails names the endpoint, in brackets, from the library's reason.
TEST(MainWindowTest, LogsWhyItIsNotListening) {
    const auto window = makeTestWindow();
    const QString endpoint = listeningEndpoint(*window);
    ASSERT_FALSE(endpoint.isEmpty());

    const auto second = makeTestWindow(endpoint);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return serverControlsOf(*second)->state() == ServerState::NotListening; }, 2000));

    EXPECT_TRUE(logOf(*second)->latest()->text.startsWith(
        QStringLiteral("Not listening: could not listen on [%1]: ").arg(endpoint)))
        << logOf(*second)->latest()->text.toStdString();
}

// Names are in brackets, sizes are not.
TEST(MainWindowTest, LogsArrivalsByName) {
    const auto window = makeTestWindow();
    const ActivityLog* log = logOf(*window);

    window->addImage("frame", makeImage());
    EXPECT_TRUE(log->latest()->text.startsWith(QStringLiteral("Received [frame] (")))
        << log->latest()->text.toStdString();
    window->addImage("photo.png", makeImage(),
                     {.source = viewer_core::ImageSource::OpenedFile, .fileBytes = 10});
    EXPECT_TRUE(log->latest()->text.startsWith(QStringLiteral("Opened [photo.png] (")))
        << log->latest()->text.toStdString();
}

TEST(MainWindowTest, LogsEditsAndEvictions) {
    const auto window = makeTestWindow();
    const ActivityLog* log = logOf(*window);
    addThree(*window);

    window->renameImage(3, QStringLiteral("renamed"));
    EXPECT_EQ(log->latest()->text, QStringLiteral("Renamed [c] to [renamed]"));
    window->duplicateImage(3);
    EXPECT_EQ(log->latest()->text, QStringLiteral("Duplicated [renamed]"));
    window->copyImage(3);
    EXPECT_EQ(log->latest()->text, QStringLiteral("Copied [renamed] to the clipboard"));
    window->deleteImage(3);
    EXPECT_EQ(log->latest()->text, QStringLiteral("Deleted [renamed]"));
    window->undoDelete();
    EXPECT_EQ(log->latest()->text, QStringLiteral("Restored [renamed]"));

    // The history keeps 100: three more than that drop the three oldest in turn.
    for (std::size_t i = 0; i < viewer_core::kDefaultHistoryCapacity; ++i) {
        window->addImage("filler", makeImage());
    }
    EXPECT_NE(findEntry(*log, QStringLiteral("Dropped the oldest image")), nullptr);
}

// The Log toggle ends the status bar, after the server's state.
TEST(MainWindowTest, LogToggleIsAtTheFarRight) {
    const auto window = makeTestWindow();
    window->show();
    auto* toggle = window->findChild<QToolButton*>(QStringLiteral("logToggle"));
    auto* status = window->findChild<QLabel*>(QStringLiteral("serverStatus"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(status, nullptr);

    EXPECT_GT(toggle->mapTo(window.get(), QPoint()).x(), status->mapTo(window.get(), QPoint()).x());
}

// The sidebar, the log and the status bar sit on the panel grey, a step off the image area's background, and
// follow a change of palette at once (spec H3).
TEST(MainWindowTest, PanelsUseThePanelGrey) {
    const test_support::PaletteGuard guard;
    const auto window = makeTestWindow();
    window->show();
    const std::vector<QWidget*> panels = {sidebarOf(*window),
                                          window->findChild<QWidget*>(QStringLiteral("activityLogPanel")),
                                          window->statusBar()};

    for (const QPalette& palette : {test_support::paletteWith(QColor(236, 236, 236), Qt::black),
                                    test_support::paletteWith(QColor(30, 30, 30), Qt::white)}) {
        test_support::applyPalette(palette);
        for (const QWidget* panel : panels) {
            EXPECT_TRUE(panel->autoFillBackground()) << panel->objectName().toStdString();
            EXPECT_EQ(panel->palette().color(QPalette::Window), qt_common::panelGrey(palette))
                << panel->objectName().toStdString();
            EXPECT_EQ(panel->palette().color(QPalette::Base), qt_common::panelGrey(palette))
                << panel->objectName().toStdString();
        }
        EXPECT_EQ(window->findChild<ImageView*>()->palette().color(QPalette::Window),
                  palette.color(QPalette::Window));
    }
}

// The splitter handles are about 7 points wide, with a divider line along the middle (spec H3).
TEST(MainWindowTest, SplitterHandlesShow) {
    const auto window = makeTestWindow();
    window->show();
    window->findChild<QToolButton*>(QStringLiteral("logToggle"))->click();
    const QPalette palette = QGuiApplication::palette();

    for (const char* name : {"mainSplitter", "logSplitter"}) {
        auto* splitter = window->findChild<QSplitter*>(QString::fromLatin1(name));
        ASSERT_NE(splitter, nullptr) << name;
        EXPECT_EQ(splitter->handleWidth(), 7) << name;
        const QImage handle = splitter->handle(1)->grab().toImage();
        const bool across = splitter->orientation() == Qt::Horizontal;
        const QPoint middle = across ? QPoint(handle.width() / 2, 2) : QPoint(2, handle.height() / 2);
        const QPoint edge = across ? QPoint(0, 2) : QPoint(2, 0);
        EXPECT_EQ(handle.pixelColor(middle), qt_common::dividerGrey(palette)) << name;
        EXPECT_EQ(handle.pixelColor(edge), qt_common::panelGrey(palette)) << name;
    }
}

// A divider line runs along the top of the status bar (spec H3).
TEST(MainWindowTest, StatusBarHasADividerOnTop) {
    const auto window = makeTestWindow();
    window->show();
    auto* divider = window->findChild<QWidget*>(QStringLiteral("statusBarDivider"));
    ASSERT_NE(divider, nullptr);

    EXPECT_EQ(divider->height(), 1);
    EXPECT_EQ(divider->mapTo(window.get(), QPoint(0, 1)).y(), window->statusBar()->y());
    EXPECT_EQ(divider->grab().toImage().pixelColor(0, 0), qt_common::dividerGrey(QGuiApplication::palette()));
}

// Large enough to see (spec H1), and in the application's regular font even where the platform gives status
// bars and tool buttons a smaller one.
TEST(MainWindowTest, StatusBarIsLargeEnoughToSee) {
    const test_support::SmallerClassFonts smaller;
    const auto window = makeTestWindow();
    window->show();
    window->addImage("a", makeImage());
    auto* toggle = window->findChild<QToolButton*>(QStringLiteral("logToggle"));

    EXPECT_TRUE(test_support::isLargeFramedButton(toggle));
    EXPECT_TRUE(test_support::usesTheRegularFont(toggle));
    EXPECT_TRUE(test_support::usesTheRegularFont(window->findChild<QLabel*>(QStringLiteral("serverStatus"))));
    EXPECT_TRUE(
        test_support::usesTheRegularFont(window->findChild<QLabel*>(QStringLiteral("latestLogEntry"))));
    EXPECT_TRUE(test_support::usesTheRegularFont(window->findChild<QLabel*>(QStringLiteral("pixelReadout"))));
}

TEST(MainWindowTest, LatestEntryShowsWhileCollapsed) {
    const auto window = makeTestWindow();
    window->show();
    auto* latest = window->findChild<QLabel*>(QStringLiteral("latestLogEntry"));
    auto* toggle = window->findChild<QToolButton*>(QStringLiteral("logToggle"));
    auto* panel = window->findChild<QWidget*>(QStringLiteral("activityLogPanel"));
    ASSERT_NE(latest, nullptr);
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(panel, nullptr);
    EXPECT_FALSE(panel->isVisible());

    window->addImage("newest", makeImage());

    const LogEntry* entry = logOf(*window)->latest();
    EXPECT_TRUE(latest->isVisible());
    EXPECT_EQ(latest->text(), clockTime(toEpochMicroseconds(entry->time)) + u' ' + entry->text);
    EXPECT_TRUE(entry->text.contains(QStringLiteral("newest")));

    toggle->click();
    EXPECT_TRUE(panel->isVisible());
    EXPECT_FALSE(latest->isVisible());
    toggle->click();
    EXPECT_FALSE(panel->isVisible());
    EXPECT_TRUE(latest->isVisible());
}

TEST(MainWindowTest, LogPanelOpenStateIsRemembered) {
    {
        const auto window = makeTestWindow();
        window->findChild<QToolButton*>(QStringLiteral("logToggle"))->click();
    }
    const auto window = makeTestWindow();
    window->show();

    EXPECT_TRUE(window->findChild<QWidget*>(QStringLiteral("activityLogPanel"))->isVisible());
    EXPECT_TRUE(window->findChild<QToolButton*>(QStringLiteral("logToggle"))->isChecked());
}

TEST(MainWindowTest, ShowLogActionTogglesPanel) {
    const auto window = makeTestWindow();
    window->show();
    QAction* showLog = action(*window, "actionShowLog");
    auto* toggle = window->findChild<QToolButton*>(QStringLiteral("logToggle"));
    auto* panel = window->findChild<QWidget*>(QStringLiteral("activityLogPanel"));
    ASSERT_NE(showLog, nullptr);
    EXPECT_TRUE(showLog->isCheckable());
    EXPECT_EQ(showLog->shortcut(), QKeySequence(QStringLiteral("Ctrl+L")));
    EXPECT_FALSE(showLog->isChecked());

    showLog->trigger();
    EXPECT_TRUE(toggle->isChecked());
    EXPECT_TRUE(panel->isVisible());

    toggle->click();
    EXPECT_FALSE(showLog->isChecked());
    EXPECT_FALSE(panel->isVisible());
}

// The titles of a menu bar's or a menu's entries, without their mnemonics; a separator reads as "-".
QStringList entryTitles(const QList<QAction*>& actions) {
    QStringList titles;
    for (const QAction* entry : actions) {
        titles << (entry->isSeparator() ? QStringLiteral("-") : entry->text().remove(u'&'));
    }
    return titles;
}

TEST(MainWindowTest, MenusAreInTheirOrder) {
    const auto window = makeTestWindow();
    EXPECT_EQ(entryTitles(window->menuBar()->actions()),
              (QStringList{QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View"),
                           QStringLiteral("Image"), QStringLiteral("Server"), QStringLiteral("History"),
                           QStringLiteral("Help")}));
    EXPECT_EQ(
        entryTitles(window->findChild<QMenu*>(QStringLiteral("menuImage"))->actions()),
        (QStringList{QStringLiteral("Range"), QStringLiteral("Colours"), QStringLiteral("Show Clipping"),
                     QStringLiteral("Show Overlays"), QStringLiteral("Show Histogram"), QStringLiteral("-"),
                     QStringLiteral("Freeze"), QStringLiteral("Snapshot")}));
    QStringList view{QStringLiteral("Show in Sidebar"), QStringLiteral("Sort By"), QStringLiteral("-"),
                     QStringLiteral("Show Log")};
    if (qt_common::canChooseAppearance()) {
        view << QStringLiteral("-") << QStringLiteral("Appearance");
    }
    EXPECT_EQ(entryTitles(window->findChild<QMenu*>(QStringLiteral("menuView"))->actions()), view);
}

// --- Display range and colours ----------------------------------------------------------------------------

DisplaySettingsControl* displayControlOf(MainWindow& window) {
    return window.findChild<DisplaySettingsControl*>();
}

QAction* entryTitled(const QMenu* menu, const QString& title) {
    for (QAction* entry : menu->actions()) {
        if (entry->text().remove(u'&') == title) {
            return entry;
        }
    }
    return nullptr;
}

QMenu* imageSubmenu(MainWindow& window, const QString& title) {
    const QAction* entry = entryTitled(window.findChild<QMenu*>(QStringLiteral("menuImage")), title);
    return entry != nullptr ? entry->menu() : nullptr;
}

QString buttonText(MainWindow& window, const char* name) {
    return displayControlOf(window)->findChild<QToolButton*>(QString::fromLatin1(name))->text();
}

void applyFixedRange(MainWindow& window, const QString& minimum, const QString& maximum) {
    DisplaySettingsControl* control = displayControlOf(window);
    entryTitled(control->rangeMenu(), QStringLiteral("Fixed…"))->trigger();
    control->findChild<QLineEdit*>(QStringLiteral("fixedMinimum"))->setText(minimum);
    control->findChild<QLineEdit*>(QStringLiteral("fixedMaximum"))->setText(maximum);
    control->findChild<QPushButton*>(QStringLiteral("applyFixedRange"))->click();
}

// The grey level at the middle of the picture in the view, which a fitted image fills.
int viewedGrey(MainWindow& window) {
    QImage view = window.findChild<ImageView*>()->grab().toImage();
    return view.pixelColor(view.width() / 2, view.height() / 2).red();
}

int thumbnailGrey(MainWindow& window, ImageId id) {
    const auto* model = panelOf(window)->findChild<ThumbnailModel*>(); // the view's own model is a filter
    if (model == nullptr) {
        return -1;
    }
    const QImage thumbnail = model->thumbnail(id, QSize(16, 16)).toImage();
    return thumbnail.pixelColor(thumbnail.width() / 2, thumbnail.height() / 2).red();
}

// (F5) The view, Copy Image, Save As and the thumbnail all show the range.
TEST(MainWindowTest, RangeChangesTheViewCopyAndTheThumbnail) {
    const auto window = makeTestWindow();
    window->resize(800, 600);
    window->show();
    window->addImage("a", cv::Mat(40, 40, CV_8UC1, cv::Scalar(100)));
    QApplication::processEvents();
    ASSERT_EQ(viewedGrey(*window), 100);

    applyFixedRange(*window, QStringLiteral("0"), QStringLiteral("50"));
    QApplication::processEvents();

    EXPECT_EQ(viewedGrey(*window), 255);
    window->copyImage(1);
    EXPECT_EQ(QGuiApplication::clipboard()->image().pixelColor(20, 20).red(), 255);
    EXPECT_EQ(thumbnailGrey(*window, 1), 255);
    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: Fixed ▾"));
}

// A 16-bit left-to-right ramp, so a colour map shows as colour.
cv::Mat ramp() {
    cv::Mat image(30, 40, CV_16UC1);
    for (int x = 0; x < image.cols; ++x) {
        image.col(x).setTo(x * 1000);
    }
    return image;
}

// Whether the middle of a thumbnail is coloured, not grey.
bool thumbnailIsColoured(MainWindow& window, ImageId id) {
    const auto* model = panelOf(window)->findChild<ThumbnailModel*>();
    const QImage thumbnail = model->thumbnail(id, QSize(40, 30)).toImage();
    const QColor middle = thumbnail.pixelColor(thumbnail.width() / 2, thumbnail.height() / 2);
    return middle.red() != middle.green() || middle.green() != middle.blue();
}

void chooseColours(MainWindow& window, int entry) { // 0 Grey, 1 Turbo, 2 Viridis
    imageSubmenu(window, QStringLiteral("Colours"))->actions().at(entry)->trigger();
}

// (F5) Every thumbnail of a name shows its colours, redrawn when they change.
TEST(MainWindowTest, ThumbnailFollowsTheDisplaySettings) {
    const auto window = makeTestWindow();
    window->addImage("g", ramp());
    ASSERT_FALSE(thumbnailIsColoured(*window, 1));

    chooseColours(*window, 1);
    EXPECT_TRUE(thumbnailIsColoured(*window, 1));
    chooseColours(*window, 0);
    EXPECT_FALSE(thumbnailIsColoured(*window, 1));
}

TEST(MainWindowTest, LaterArrivalOfTheNameGetsItsSettings) {
    const auto window = makeTestWindow();
    window->addImage("g", ramp());
    chooseColours(*window, 1);

    window->addImage("g", ramp());

    EXPECT_TRUE(thumbnailIsColoured(*window, 2));
}

TEST(MainWindowTest, SettingsFollowTheName) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    imageSubmenu(*window, QStringLiteral("Colours"))->actions().at(1)->trigger(); // Turbo
    window->addImage("b", makeImage());
    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Grey ▾"));

    panelOf(*window)->selectImage(1);
    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Turbo ▾"));
    window->addImage("a", makeImage()); // another image of the same name
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(3));
    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Turbo ▾"));
}

TEST(MainWindowTest, RenameCarriesTheSettings) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    imageSubmenu(*window, QStringLiteral("Colours"))->actions().at(2)->trigger(); // Viridis

    window->renameImage(1, QStringLiteral("z"));
    window->addImage("a", makeImage());
    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Grey ▾"));

    panelOf(*window)->selectImage(1);
    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Viridis ▾"));
}

// (F5) A rename moves the settings from one name to another, so the thumbnails of both names change: the
// image left with the old name turns grey, and one that already had the new name takes its settings.
TEST(MainWindowTest, RenameRefreshesTheThumbnailsOfBothNames) {
    const auto window = makeTestWindow();
    window->addImage("a", ramp());
    window->addImage("a", ramp());
    window->addImage("z", ramp());
    chooseColours(*window, 1); // Turbo for "z"
    panelOf(*window)->selectImage(1);
    chooseColours(*window, 2); // Viridis for "a"
    const auto* model = panelOf(*window)->findChild<ThumbnailModel*>();
    const auto middleOf = [model](ImageId id) {
        const QImage thumbnail = model->thumbnail(id, QSize(40, 30)).toImage();
        return thumbnail.pixelColor(thumbnail.width() / 2, thumbnail.height() / 2);
    };
    ASSERT_TRUE(thumbnailIsColoured(*window, 2));
    ASSERT_NE(middleOf(3), middleOf(1)); // drawn, in Turbo and Viridis

    window->renameImage(1, QStringLiteral("z"));

    EXPECT_FALSE(thumbnailIsColoured(*window, 2));
    EXPECT_EQ(middleOf(3), middleOf(1));
}

TEST(MainWindowTest, ImageMenuMirrorsTheControl) {
    const auto window = makeTestWindow();
    DisplaySettingsControl* control = displayControlOf(*window);
    QMenu* range = imageSubmenu(*window, QStringLiteral("Range"));
    QMenu* colours = imageSubmenu(*window, QStringLiteral("Colours"));
    ASSERT_NE(range, nullptr);
    ASSERT_NE(colours, nullptr);
    EXPECT_NE(range, control->rangeMenu()); // a native menu has one parent
    // The control's Range menu ends with Show Clipping, which the Image menu lists beside its Range menu.
    QList<QAction*> rangeModes = control->rangeMenu()->actions();
    ASSERT_GE(rangeModes.size(), 2);
    EXPECT_EQ(rangeModes.takeLast(), action(*window, "actionShowClipping"));
    EXPECT_TRUE(rangeModes.takeLast()->isSeparator());
    EXPECT_EQ(range->actions(), rangeModes);
    EXPECT_EQ(colours->actions(), control->coloursMenu()->actions());
    EXPECT_FALSE(window->findChild<QMenu*>(QStringLiteral("menuImage"))->isEnabled()); // nothing shown

    window->addImage("colour", cv::Mat(3, 4, CV_8UC3, cv::Scalar(1, 2, 3)));
    EXPECT_TRUE(window->findChild<QMenu*>(QStringLiteral("menuImage"))->isEnabled());
    EXPECT_FALSE(colours->actions().at(1)->isEnabled()); // colour maps are for single-channel images

    window->addImage("grey", makeImage());
    colours->actions().at(1)->trigger();
    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Turbo ▾"));
}

// Not started: the event loop does not run, so the fixed port is never bound.
TEST(MainWindowTest, LogSurvivesRestart) {
    const QTemporaryDir root;
    const QString endpoint = QStringLiteral("tcp://127.0.0.1:47139");
    makeTestWindow(endpoint, root.path())->addImage("kept", makeImage());

    const auto window = makeTestWindow(endpoint, root.path());

    EXPECT_NE(findEntry(*logOf(*window), QStringLiteral("kept")), nullptr);
}

TEST(MainWindowTest, SaysWhenTheLogIsMemoryOnly) {
    const auto wildcard = makeTestWindow();
    EXPECT_EQ(logOf(*wildcard)->latest()->text,
              QStringLiteral("Not keeping images or the log: the port changes on every start"));
    EXPECT_EQ(logOf(*wildcard)->latest()->kind, LogKind::Warning);

    const QTemporaryDir root;
    const QString endpoint = QStringLiteral("tcp://127.0.0.1:47139");
    const auto first = makeTestWindow(endpoint, root.path());
    const auto second = makeTestWindow(endpoint, root.path());
    EXPECT_EQ(logOf(*first)->rowCount(), 0);
    EXPECT_EQ(logOf(*second)->latest()->text,
              QStringLiteral("Another viewer uses this endpoint's data folder: keeping the log and images in "
                             "memory only"));

    // A folder cannot be made under a file.
    QFile blocker(QDir(root.path()).filePath(QStringLiteral("file")));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const QString unusable = blocker.fileName() + QStringLiteral("/root");
    const auto third = makeTestWindow(endpoint, unusable);
    EXPECT_EQ(logOf(*third)->latest()->text,
              QStringLiteral("Cannot use the data folder in [%1]: keeping the log and images in memory only")
                  .arg(QDir::toNativeSeparators(unusable)));
}

TEST(MainWindowTest, CopyAndClearLog) {
    const auto window = makeTestWindow();
    window->show();
    window->findChild<QToolButton*>(QStringLiteral("logToggle"))->click();
    addThree(*window);
    ActivityLog* log = logOf(*window);
    auto* list = window->findChild<QListView*>(QStringLiteral("logList"));
    ASSERT_NE(list, nullptr);

    window->findChild<QToolButton*>(QStringLiteral("logCopy"))->click();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), log->toPlainText({}));
    list->selectionModel()->select(log->index(1), QItemSelectionModel::ClearAndSelect);
    window->findChild<QToolButton*>(QStringLiteral("logCopy"))->click();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), log->data(log->index(1), Qt::DisplayRole).toString());

    window->findChild<QToolButton*>(QStringLiteral("logClear"))->click();
    EXPECT_EQ(log->rowCount(), 0);
    EXPECT_TRUE(window->findChild<QLabel*>(QStringLiteral("latestLogEntry"))->text().isEmpty());
}

// --- Keeping images between sessions ----------------------------------------------------------------------

// A fixed port, so that a restarted window finds the same data folder. The window listens on it once its
// event loop runs, but these tests only look at storage, so they pass whether or not that succeeds (another
// process may hold the port).
QString unusedEndpoint() { return QStringLiteral("tcp://127.0.0.1:47141"); }

QString imagesFolderOf(const QString& root, const QString& endpoint = unusedEndpoint()) {
    return QDir(root).filePath(DataFolder::folderNameFor(endpoint) + QStringLiteral("/images"));
}

int storedFiles(const QString& root, const QString& endpoint = unusedEndpoint()) {
    return ImageStore(imagesFolderOf(root, endpoint)).storedImageCount();
}

bool waitForStoredFiles(const QString& root, int count) {
    return QTest::qWaitFor([&] { return storedFiles(root) == count; }, 5000);
}

// A window on the unused endpoint once it has restored what was stored, or null if it never finishes.
std::unique_ptr<MainWindow> makeRestoredWindow(const QString& root) {
    auto window = makeTestWindow(unusedEndpoint(), root);
    QSignalSpy restored(window.get(), &MainWindow::restoreFinished);
    return restored.wait(5000) ? std::move(window) : nullptr;
}

std::vector<QString> listedNames(MainWindow& window) {
    std::vector<QString> names;
    for (const ImageId id : panelOf(window)->listedIds()) {
        names.push_back(listedName(window, id));
    }
    return names;
}

qint64 listedModifiedTime(MainWindow& window, ImageId id) {
    const QAbstractItemModel* model = panelOf(window)->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(ThumbnailModel::IdRole).value<quint64>() == id) {
            return index.data(ThumbnailModel::ModifiedRole).toLongLong();
        }
    }
    return -1;
}

// The 8-bit picture Copy Image puts on the clipboard.
QImage copiedPicture(MainWindow& window, ImageId id) {
    window.copyImage(id);
    return QGuiApplication::clipboard()->image();
}

cv::Mat randomImage() {
    cv::Mat image(6, 8, CV_8UC1);
    cv::randu(image, cv::Scalar::all(0), cv::Scalar::all(255));
    return image;
}

TEST(MainWindowTest, KeptImagesComeBackAfterRestart) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    QImage pictureA;
    QImage pictureB;
    qint64 renamedAt = 0;
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("a", randomImage());
        window->addImage("b", randomImage());
        window->renameImage(2, QStringLiteral("bee"));
        pictureA = copiedPicture(*window, 1);
        pictureB = copiedPicture(*window, 2);
        renamedAt = listedModifiedTime(*window, 2);
    }

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);

    ASSERT_EQ(listedNames(*window), (std::vector<QString>{QStringLiteral("bee"), QStringLiteral("a")}));
    const std::vector<ImageId> ids = panelOf(*window)->listedIds();
    EXPECT_EQ(copiedPicture(*window, ids[0]), pictureB);
    EXPECT_EQ(copiedPicture(*window, ids[1]), pictureA);
    EXPECT_EQ(listedModifiedTime(*window, ids[0]), renamedAt);
    EXPECT_NE(findEntry(*logOf(*window), QStringLiteral("Restored 2 images")), nullptr);
}

TEST(MainWindowTest, ImagesAreNotStoredByDefault) {
    const QTemporaryDir root;
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("a", makeImage());
    }

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);

    EXPECT_TRUE(panelOf(*window)->listedIds().empty());
    EXPECT_EQ(storedFiles(root.path()), 0);
}

TEST(MainWindowTest, ArrivalsDuringLoadStayNewest) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("a", makeImage());
        window->addImage("b", makeImage());
    }
    {
        const auto window = makeTestWindow(unusedEndpoint(), root.path());
        QSignalSpy restored(window.get(), &MainWindow::restoreFinished);
        window->addImage("c", makeImage()); // before the event loop has started loading

        ASSERT_TRUE(restored.wait(5000));
        EXPECT_EQ(listedNames(*window),
                  (std::vector<QString>{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")}));
    }
    // The arrival is stored too, at the top.
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(listedNames(*window),
              (std::vector<QString>{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")}));
}

TEST(MainWindowTest, TurningOffDeletesEveryEndpointsImages) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    const QString otherEndpoint = QStringLiteral("tcp://127.0.0.1:47142");
    {
        const auto other = DataFolder::open(root.path(), otherEndpoint);
        ASSERT_NE(other, nullptr);
        ImageStore store(other->imagesFolder());
        store.write("b1", makeImage());
        store.write("b2", makeImage());
    }
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    window->addImage("a", makeImage());
    ASSERT_TRUE(waitForStoredFiles(root.path(), 1));
    QString asked;
    window->setConfirmationForTests([&asked](const QString& text) {
        asked = text;
        return true;
    });

    action(*window, "actionKeepImages")->trigger();

    EXPECT_EQ(asked, QStringLiteral("Delete the 3 stored images?"));
    EXPECT_FALSE(action(*window, "actionKeepImages")->isChecked());
    EXPECT_FALSE(keepImagesSetting());
    EXPECT_EQ(storedFiles(root.path(), otherEndpoint), 0);
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return QDir(imagesFolderOf(root.path())).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty();
        },
        5000));
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1)); // the history itself stays
}

TEST(MainWindowTest, CancellingTurnOffKeepsSetting) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    window->addImage("a", makeImage());
    ASSERT_TRUE(waitForStoredFiles(root.path(), 1));
    window->setConfirmationForTests([](const QString&) { return false; });

    action(*window, "actionKeepImages")->trigger();

    EXPECT_TRUE(action(*window, "actionKeepImages")->isChecked());
    EXPECT_TRUE(keepImagesSetting());
    QTest::qWait(50);
    EXPECT_EQ(storedFiles(root.path()), 1);
}

TEST(MainWindowTest, TurningOnStoresTheCurrentHistory) {
    const QTemporaryDir root;
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    window->addImage("a", makeImage());
    window->addImage("b", makeImage());

    action(*window, "actionKeepImages")->trigger();

    EXPECT_TRUE(keepImagesSetting());
    EXPECT_TRUE(waitForStoredFiles(root.path(), 2));
}

TEST(MainWindowTest, StartWithSettingOffCleansUnlockedLeftovers) {
    const QTemporaryDir root;
    const QString releasedEndpoint = QStringLiteral("tcp://127.0.0.1:47142");
    {
        const auto released = DataFolder::open(root.path(), releasedEndpoint);
        ASSERT_NE(released, nullptr);
        ImageStore(released->imagesFolder()).write("left over", makeImage());
    }
    const QString heldEndpoint = QStringLiteral("tcp://127.0.0.1:47143");
    const auto held = DataFolder::open(root.path(), heldEndpoint); // another viewer, still running
    ASSERT_NE(held, nullptr);
    ImageStore(held->imagesFolder()).write("in use", makeImage());

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);

    EXPECT_EQ(storedFiles(root.path(), releasedEndpoint), 0);
    EXPECT_EQ(storedFiles(root.path(), heldEndpoint), 1);
}

TEST(MainWindowTest, DeleteRemovesFileAndUndoRestoresIt) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("a", makeImage());
        ASSERT_TRUE(waitForStoredFiles(root.path(), 1));

        window->deleteImage(1);
        EXPECT_TRUE(waitForStoredFiles(root.path(), 0));
        window->undoDelete();
        EXPECT_TRUE(waitForStoredFiles(root.path(), 1));
    }
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(listedNames(*window), std::vector<QString>{QStringLiteral("a")});
}

TEST(MainWindowTest, WildcardEndpointStoresNothing) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    const auto window = makeTestWindow(QStringLiteral("tcp://127.0.0.1:*"), root.path());

    window->addImage("a", makeImage());
    QTest::qWait(100);

    EXPECT_TRUE(QDir(root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    EXPECT_NE(findEntry(*logOf(*window),
                        QStringLiteral("Not keeping images or the log: the port changes on every start")),
              nullptr);
}

TEST(MainWindowTest, ClearHistoryEmptiesMemoryAndDisk) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    window->addImage("a", makeImage());
    window->addImage("b", makeImage());
    window->deleteImage(2);
    ASSERT_TRUE(waitForStoredFiles(root.path(), 1));
    QString asked;
    window->setConfirmationForTests([&asked](const QString& text) {
        asked = text;
        return true;
    });

    action(*window, "actionClearHistory")->trigger();

    EXPECT_EQ(asked, QStringLiteral("Delete the image in the history? This cannot be undone."));
    EXPECT_TRUE(panelOf(*window)->listedIds().empty());
    EXPECT_EQ(window->shownImageId(), std::nullopt);
    EXPECT_FALSE(action(*window, "actionUndoDelete")->isEnabled());
    EXPECT_TRUE(waitForStoredFiles(root.path(), 0));
}

// A clear while stored images are still loading clears those too: they never come back, now or at the next
// start, and the index names no deleted file.
TEST(MainWindowTest, ClearHistoryDuringRestoreStaysCleared) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("a", makeImage());
        window->addImage("b", makeImage());
    }
    {
        const auto window = makeTestWindow(unusedEndpoint(), root.path());
        QSignalSpy restored(window.get(), &MainWindow::restoreFinished);
        window->setConfirmationForTests([](const QString&) { return true; });
        window->addImage("c", makeImage());
        QCoreApplication::processEvents(); // starts the loading, whose images arrive after the clear

        action(*window, "actionClearHistory")->trigger();

        ASSERT_TRUE(restored.count() > 0 || restored.wait(5000));
        EXPECT_TRUE(panelOf(*window)->listedIds().empty());
    }
    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    EXPECT_TRUE(panelOf(*window)->listedIds().empty());
    EXPECT_EQ(findEntry(*logOf(*window), QStringLiteral("damaged")), nullptr);
    EXPECT_EQ(storedFiles(root.path()), 0);
}

// --- Live channels ----------------------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// A window on a clock the test moves, so channels go idle and rates change without waiting.
struct ClockedWindow {
    explicit ClockedWindow(LiveConversion conversion = LiveConversion::Immediate)
        : window(test_support::makeClockedTestWindow([this] { return now; }, conversion)) {}

    Clock::time_point now = Clock::now();
    std::unique_ptr<MainWindow> window;
};

cv::Mat liveFrame(int value, cv::Size size = {40, 30}) { return cv::Mat(size, CV_8UC1, cv::Scalar(value)); }

// Hands a live frame to the window's sink as the server's thread would; the window takes it once events run.
visual_debug::Acceptance
feedLive(MainWindow& window, const std::string& channel, const cv::Mat& frame,
         std::chrono::system_clock::time_point receivedAt = std::chrono::system_clock::now(),
         std::optional<visual_debug::SentTime> sentAt = std::nullopt,
         const visual_debug::ImageDescription& description = {}) {
    return window.sinkForTests().onImageReceived({.name = channel,
                                                  .image = frame,
                                                  .receivedAt = receivedAt,
                                                  .live = true,
                                                  .sentAt = sentAt,
                                                  .description = description});
}

LiveChannelPanel* livePanelOf(MainWindow& window) { return sidebarOf(window)->liveChannels(); }

QWidget* liveRowOf(MainWindow& window, const QString& name) {
    return livePanelOf(window)->findChild<QWidget*>(QStringLiteral("liveRow_") + name);
}

void chooseLiveRow(MainWindow& window, const QString& name) {
    QTest::mouseClick(liveRowOf(window, name), Qt::LeftButton);
}

void clickMute(MainWindow& window, const QString& name) {
    liveRowOf(window, name)->findChild<QToolButton*>(QStringLiteral("liveMute"))->click();
}

QString readoutText(MainWindow& window) {
    return window.findChild<QLabel*>(QStringLiteral("pixelReadout"))->text();
}

QString infoDetails(MainWindow& window) {
    return infoBarOf(window)->findChild<QLabel*>(QStringLiteral("imageDetails"))->text();
}

void showSized(MainWindow& window) {
    window.resize(800, 600);
    window.show();
    QApplication::processEvents();
}

void moveMouseOver(QWidget& widget, QPointF position) {
    QMouseEvent event(QEvent::MouseMove, position, widget.mapToGlobal(position), Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

void zoomIn(QWidget& widget, QPointF position) {
    QWheelEvent event(position, widget.mapToGlobal(position), QPoint(), QPoint(0, 4 * 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&widget, &event);
}

TEST(MainWindowTest, LiveGroupAppearsAndDisappears) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    LiveChannelPanel* live = livePanelOf(window);
    EXPECT_TRUE(live->isHidden());

    ASSERT_EQ(feedLive(window, "camera", liveFrame(1)), visual_debug::Acceptance::Taken);
    QApplication::processEvents();
    EXPECT_FALSE(live->isHidden());
    EXPECT_EQ(live->channelNames(), QStringList{QStringLiteral("camera")});

    clocked.now += 6s;
    window.tick(clocked.now);
    auto* close =
        liveRowOf(window, QStringLiteral("camera"))->findChild<QToolButton*>(QStringLiteral("liveClose"));
    ASSERT_FALSE(close->isHidden());
    close->click();

    EXPECT_TRUE(live->isHidden());
    EXPECT_EQ(window.shownLiveChannel(), std::nullopt);
    EXPECT_EQ(logOf(window)->latest()->text, QStringLiteral("Closed live channel [camera]"));
    // The sink forgot it too: the next frame starts it again.
    feedLive(window, "camera", liveFrame(2));
    QApplication::processEvents();
    EXPECT_EQ(countEntries(*logOf(window), QStringLiteral("Live channel [camera] started")), 2);
}

TEST(MainWindowTest, FirstChannelIsShownAutomatically) {
    const auto window = makeTestWindow();
    feedLive(*window, "camera", liveFrame(1));
    feedLive(*window, "edges", liveFrame(2));
    QApplication::processEvents();

    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));
    EXPECT_EQ(window->shownImageId(), std::nullopt);
    EXPECT_EQ(livePanelOf(*window)->selectedChannel(), QStringLiteral("camera"));
    EXPECT_TRUE(window->windowTitle().contains(QStringLiteral("camera")));
    EXPECT_FALSE(infoBarOf(*window)->isHidden());
    EXPECT_NE(findEntry(*logOf(*window), QStringLiteral("Live channel [camera] started")), nullptr);
    EXPECT_NE(findEntry(*logOf(*window), QStringLiteral("Live channel [edges] started")), nullptr);

    // With an image in view, a channel is listed but not shown.
    const auto busy = makeTestWindow();
    busy->addImage("a", makeImage());
    feedLive(*busy, "camera", liveFrame(1));
    QApplication::processEvents();
    EXPECT_EQ(busy->shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(busy->shownLiveChannel(), std::nullopt);
}

TEST(MainWindowTest, ShowsTheNewestFrameOfAFlood) {
    const auto window = makeTestWindow();
    showSized(*window);

    // The UI runs no events between the frames, as when a sender outpaces it.
    for (int i = 0; i < 200; ++i) {
        ASSERT_EQ(feedLive(*window, "camera", liveFrame(i)), visual_debug::Acceptance::Taken);
    }
    QApplication::processEvents();

    EXPECT_EQ(viewedGrey(*window), 199);
    EXPECT_EQ(countEntries(*logOf(*window), QStringLiteral("camera")), 1); // started, and no line per frame
    window->tick(Clock::now());
    EXPECT_TRUE(infoDetails(*window).contains(QStringLiteral("199 dropped")))
        << infoDetails(*window).toStdString();
}

TEST(MainWindowTest, KeepsZoomAcrossSameSizeFrames) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();
    const QPointF centre = QRectF(view->rect()).center();
    zoomIn(*view, centre);
    moveMouseOver(*view, centre + QPointF(40, 40));
    const QString before = readoutText(*window).section(QStringLiteral(" ·"), 0, 0);
    ASSERT_FALSE(before.isEmpty());

    feedLive(*window, "camera", liveFrame(2));
    QApplication::processEvents();

    EXPECT_EQ(readoutText(*window).section(QStringLiteral(" ·"), 0, 0), before);
}

TEST(MainWindowTest, RefitsWhenTheFrameSizeChanges) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();
    const QPointF centre = QRectF(view->rect()).center();
    zoomIn(*view, centre);
    moveMouseOver(*view, centre + QPointF(40, 40));
    const QString before = readoutText(*window).section(QStringLiteral(" ·"), 0, 0);

    feedLive(*window, "camera", liveFrame(2, {80, 60}));
    QApplication::processEvents();

    EXPECT_NE(readoutText(*window).section(QStringLiteral(" ·"), 0, 0), before);
}

TEST(MainWindowTest, ReadoutFollowsTheLiveFrame) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();
    moveMouseOver(*view, QRectF(view->rect()).center());
    EXPECT_TRUE(readoutText(*window).endsWith(QStringLiteral("I=1"))) << readoutText(*window).toStdString();

    feedLive(*window, "camera", liveFrame(7));
    QApplication::processEvents();

    EXPECT_TRUE(readoutText(*window).endsWith(QStringLiteral("I=7"))) << readoutText(*window).toStdString();
}

TEST(MainWindowTest, InfoStripShowsTheCounters) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    const auto receivedAt = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const visual_debug::SentTime sentAt = receivedAt - 18ms;
    for (int i = 0; i < 3; ++i) { // one taken, two replaced in the sink
        feedLive(window, "camera", liveFrame(i), receivedAt, sentAt);
    }
    QApplication::processEvents();
    feedLive(window, "camera", liveFrame(3), receivedAt, sentAt);
    QApplication::processEvents();

    window.tick(clocked.now);

    EXPECT_EQ(infoBarOf(window)->findChild<QLabel*>(QStringLiteral("imageName"))->text(),
              QStringLiteral("camera"));
    EXPECT_EQ(infoDetails(window),
              QString::fromUtf8("live · 40×30 · 1 ch · 8U · 4 fps in · 2 drawn · 2 dropped · 18 ms"));
    EXPECT_FALSE(infoBarOf(window)->findChild<QToolButton*>(QStringLiteral("renameButton"))->isVisible());
}

TEST(MainWindowTest, StillAndLiveOfTheSameNameStayApart) {
    const auto window = makeTestWindow();
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();

    window->addImage("camera", makeImage());

    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));
    EXPECT_EQ(listedName(*window, 1), QStringLiteral("camera"));
    EXPECT_EQ(livePanelOf(*window)->channelNames(), QStringList{QStringLiteral("camera")});
    ASSERT_TRUE(panelOf(*window)->selectImage(1));
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1));
    EXPECT_FALSE(infoDetails(*window).startsWith(QStringLiteral("live")));
    feedLive(*window, "camera", liveFrame(2));
    QApplication::processEvents();
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1)); // a frame never takes the view
}

// (Q1) While a channel is watched, stills are listed, not shown, so the stream stays in view.
TEST(MainWindowTest, StillArrivalKeepsTheChannelShown) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();
    chooseLiveRow(*window, QStringLiteral("camera"));

    window->addImage("b", makeImage());

    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));
    EXPECT_EQ(window->shownImageId(), std::nullopt);
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::nullopt);
    EXPECT_EQ(listedName(*window, 2), QStringLiteral("b"));
}

TEST(MainWindowTest, ReclickingTheLastThumbnailLeavesTheChannel) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();
    chooseLiveRow(*window, QStringLiteral("camera"));
    ASSERT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));

    panelOf(*window)->setCurrentIndex(panelOf(*window)->model()->index(0, 0)); // a click on "a"

    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(window->shownLiveChannel(), std::nullopt);
}

TEST(MainWindowTest, OneSelectionAtATime) {
    const auto window = makeTestWindow();
    window->addImage("a", makeImage());
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();

    chooseLiveRow(*window, QStringLiteral("camera"));
    EXPECT_EQ(livePanelOf(*window)->selectedChannel(), QStringLiteral("camera"));
    EXPECT_TRUE(panelOf(*window)->selectionModel()->selectedIndexes().isEmpty());
    EXPECT_FALSE(action(*window, "actionRename")->isEnabled());
    EXPECT_FALSE(action(*window, "actionDuplicate")->isEnabled());
    EXPECT_FALSE(action(*window, "actionDelete")->isEnabled());
    EXPECT_TRUE(window->findChild<QMenu*>(QStringLiteral("menuImage"))->isEnabled());

    ASSERT_TRUE(panelOf(*window)->selectImage(1));
    EXPECT_EQ(livePanelOf(*window)->selectedChannel(), std::nullopt);
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::optional<ImageId>(1));
    EXPECT_TRUE(action(*window, "actionRename")->isEnabled());
}

TEST(MainWindowTest, ClosingTheShownChannelReturnsToTheHistory) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    window.addImage("a", makeImage());
    feedLive(window, "camera", liveFrame(1));
    QApplication::processEvents();
    chooseLiveRow(window, QStringLiteral("camera"));
    clocked.now += 6s;
    window.tick(clocked.now);

    liveRowOf(window, QStringLiteral("camera"))
        ->findChild<QToolButton*>(QStringLiteral("liveClose"))
        ->click();

    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(window)->currentImageId(), std::optional<ImageId>(1));

    // With no image to go back to, nothing is shown.
    ClockedWindow empty;
    feedLive(*empty.window, "camera", liveFrame(1));
    QApplication::processEvents();
    empty.now += 6s;
    empty.window->tick(empty.now);
    liveRowOf(*empty.window, QStringLiteral("camera"))
        ->findChild<QToolButton*>(QStringLiteral("liveClose"))
        ->click();
    EXPECT_EQ(empty.window->shownLiveChannel(), std::nullopt);
    EXPECT_FALSE(empty.window->findChild<ImageView*>()->hasImage());
    EXPECT_TRUE(infoBarOf(*empty.window)->isHidden());
}

// (F3) Freezing a channel and then losing its sender leaves a row that can still be closed.
TEST(MainWindowTest, FrozenIdleChannelCanBeClosed) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    feedLive(window, "camera", liveFrame(1));
    QApplication::processEvents();
    ASSERT_EQ(window.shownLiveChannel(), std::optional<std::string>("camera"));
    window.findChild<QAction*>(QStringLiteral("actionFreeze"))->trigger();
    clocked.now += viewer_core::kIdleAfter;
    window.tick(clocked.now);
    QWidget* row = liveRowOf(window, QStringLiteral("camera"));
    EXPECT_EQ(row->findChild<QLabel*>(QStringLiteral("liveState"))->text(), QStringLiteral("frozen · idle"));
    auto* close = row->findChild<QToolButton*>(QStringLiteral("liveClose"));
    ASSERT_FALSE(close->isHidden());

    close->click();

    EXPECT_NE(findEntry(*logOf(window), QStringLiteral("Closed live channel [camera]")), nullptr);
    EXPECT_EQ(window.shownLiveChannel(), std::nullopt);
}

TEST(MainWindowTest, GoesIdleInTheWindow) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    for (int i = 0; i < 3; ++i) {
        feedLive(window, "camera", liveFrame(i));
    }
    QApplication::processEvents();
    const ActivityLog* log = logOf(window);

    window.tick(clocked.now + viewer_core::kIdleAfter - 1s);
    EXPECT_EQ(countEntries(*log, QStringLiteral("idle")), 0);
    clocked.now += 6s;
    window.tick(clocked.now);
    window.tick(clocked.now + 1s);

    EXPECT_EQ(countEntries(*log, QStringLiteral("idle")), 1);
    EXPECT_NE(findEntry(*log, QStringLiteral("Live channel [camera] idle after 3 frames (2 dropped)")),
              nullptr);
    EXPECT_EQ(
        liveRowOf(window, QStringLiteral("camera"))->findChild<QLabel*>(QStringLiteral("liveState"))->text(),
        QStringLiteral("idle"));
    EXPECT_TRUE(infoDetails(window).startsWith(QStringLiteral("idle · ")))
        << infoDetails(window).toStdString();

    feedLive(window, "camera", liveFrame(9));
    QApplication::processEvents();
    window.tick(clocked.now);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Live channel [camera] started")), 2);
    EXPECT_EQ(
        liveRowOf(window, QStringLiteral("camera"))->findChild<QLabel*>(QStringLiteral("liveState"))->text(),
        QStringLiteral("live"));
}

// Each "started" line has its own "idle after" line, counting the frames since it.
TEST(MainWindowTest, IdleLineCountsTheFramesSinceItStarted) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    for (int i = 0; i < 3; ++i) {
        feedLive(window, "camera", liveFrame(i));
    }
    QApplication::processEvents();
    clocked.now += 6s;
    window.tick(clocked.now);

    for (int i = 0; i < 2; ++i) {
        feedLive(window, "camera", liveFrame(i));
        QApplication::processEvents();
    }
    clocked.now += 6s;
    window.tick(clocked.now);

    const ActivityLog* log = logOf(window);
    EXPECT_NE(findEntry(*log, QStringLiteral("Live channel [camera] idle after 3 frames (2 dropped)")),
              nullptr);
    EXPECT_NE(findEntry(*log, QStringLiteral("Live channel [camera] idle after 2 frames (0 dropped)")),
              nullptr);
}

TEST(MainWindowTest, NinthChannelIsLoggedOncePerName) {
    const auto window = makeTestWindow();
    for (int i = 1; i <= 8; ++i) {
        ASSERT_EQ(feedLive(*window, "cam" + std::to_string(i), liveFrame(i)),
                  visual_debug::Acceptance::Taken);
    }
    QApplication::processEvents();

    EXPECT_EQ(feedLive(*window, "cam9", liveFrame(9)), visual_debug::Acceptance::TooManyLiveChannels);
    QApplication::processEvents();
    for (int i = 0; i < 5; ++i) {
        feedLive(*window, "cam9", liveFrame(9));
        QApplication::processEvents();
    }
    // The server counts each refusal as a rejection too; the line above already says it.
    window->sinkForTests().onRequestRejected("too many live channels");
    QApplication::processEvents();

    const ActivityLog* log = logOf(*window);
    EXPECT_EQ(countEntries(*log, QStringLiteral("cam9")), 1);
    EXPECT_NE(findEntry(*log, QStringLiteral("Refused live channel [cam9]: 8 channels are open")), nullptr);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Rejected")), 0);
    EXPECT_EQ(livePanelOf(*window)->channelNames().size(), 8);
}

// A sender that numbers its channel names makes every frame a new channel: the log names the first few it
// refuses and counts the rest, coalesced like other rejections.
TEST(MainWindowTest, NumberedChannelNamesDoNotFloodTheLog) {
    const auto window = makeTestWindow();
    for (int i = 1; i <= 8; ++i) {
        ASSERT_EQ(feedLive(*window, "cam" + std::to_string(i), liveFrame(i)),
                  visual_debug::Acceptance::Taken);
    }
    QApplication::processEvents();

    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(feedLive(*window, "frame " + std::to_string(i), liveFrame(i)),
                  visual_debug::Acceptance::TooManyLiveChannels);
        window->sinkForTests().onRequestRejected("too many live channels"); // as the server does
        QApplication::processEvents();
    }

    const ActivityLog* log = logOf(*window);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Refused live channel")), 8);
    EXPECT_EQ(countEntries(*log, QStringLiteral("[too many live channels]")), 1);
}

// Every depth the wire format takes, in frames taller than a row, so each row's picture is shrunk.
TEST(MainWindowTest, LiveRowsTakeEveryDepth) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    for (const int depth : {CV_8U, CV_8S, CV_16U, CV_16S, CV_32S, CV_32F, CV_64F}) {
        ASSERT_EQ(feedLive(window, "depth " + std::to_string(depth),
                           cv::Mat(480, 640, CV_MAKETYPE(depth, 1), cv::Scalar(depth))),
                  visual_debug::Acceptance::Taken)
            << depth;
        QApplication::processEvents();
    }
    clocked.now += 1s;
    window.tick(clocked.now);

    EXPECT_EQ(livePanelOf(window)->channelNames().size(), 7);
}

// (Q3) A ninth channel takes the place of one that has gone idle.
TEST(MainWindowTest, IdleChannelMakesRoomInTheWindow) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    window.addImage("a", makeImage()); // in view, so no channel is
    for (int i = 1; i <= 8; ++i) {
        feedLive(window, "cam" + std::to_string(i), liveFrame(i));
    }
    QApplication::processEvents();
    clocked.now += 6s;

    EXPECT_EQ(feedLive(window, "cam9", liveFrame(9)), visual_debug::Acceptance::Taken);
    QApplication::processEvents();

    EXPECT_NE(findEntry(*logOf(window), QStringLiteral("Live channel [cam9] replaced idle channel [cam1]")),
              nullptr);
    const QStringList names = livePanelOf(window)->channelNames();
    EXPECT_FALSE(names.contains(QStringLiteral("cam1")));
    EXPECT_TRUE(names.contains(QStringLiteral("cam9")));
    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));
}

// (R2) The channel in view, frozen on a frame someone is studying, is never the one closed to make room.
TEST(MainWindowTest, ChannelInViewIsNeverReplaced) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    for (int i = 1; i <= 8; ++i) {
        feedLive(window, "cam" + std::to_string(i), liveFrame(i));
    }
    QApplication::processEvents();
    ASSERT_EQ(window.shownLiveChannel(), std::optional<std::string>("cam1"));
    action(window, "actionFreeze")->trigger();
    clocked.now += viewer_core::kIdleAfter;

    EXPECT_EQ(feedLive(window, "cam9", liveFrame(9)), visual_debug::Acceptance::Taken);
    QApplication::processEvents();

    EXPECT_NE(findEntry(*logOf(window), QStringLiteral("Live channel [cam9] replaced idle channel [cam2]")),
              nullptr);
    EXPECT_EQ(window.shownLiveChannel(), std::optional<std::string>("cam1"));
    EXPECT_TRUE(action(window, "actionFreeze")->isChecked());
}

// --- Freeze and Snapshot ----------------------------------------------------------------------------------

// A window that shows live channel "camera", whose frame is liveFrame(value), received at `receivedAt`.
void showCamera(MainWindow& window, int value,
                std::chrono::system_clock::time_point receivedAt = std::chrono::system_clock::now()) {
    feedLive(window, "camera", liveFrame(value), receivedAt);
    QApplication::processEvents();
    if (window.shownLiveChannel() != std::optional<std::string>("camera")) {
        chooseLiveRow(window, QStringLiteral("camera"));
    }
}

void showActive(MainWindow& window) {
    window.resize(800, 600);
    window.show();
    window.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window));
}

TEST(MainWindowTest, LiveActionsOnlyWhileAChannelIsShown) {
    const auto window = makeTestWindow();
    QAction* freeze = action(*window, "actionFreeze");
    QAction* snapshot = action(*window, "actionSnapshot");
    ASSERT_NE(freeze, nullptr);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_FALSE(freeze->isEnabled());
    EXPECT_FALSE(snapshot->isEnabled());
    EXPECT_TRUE(freeze->isCheckable());
    EXPECT_EQ(freeze->shortcuts(), (QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+T")),
                                                        QKeySequence(Qt::Key_Space)}));
    EXPECT_EQ(snapshot->shortcut(), QKeySequence(QStringLiteral("Ctrl+T")));
    window->addImage("a", makeImage());
    EXPECT_FALSE(freeze->isEnabled());

    showCamera(*window, 1);
    EXPECT_TRUE(freeze->isEnabled());
    EXPECT_TRUE(snapshot->isEnabled());
    auto* freezeButton = infoBarOf(*window)->findChild<QToolButton*>(QStringLiteral("freezeButton"));
    auto* snapshotButton = infoBarOf(*window)->findChild<QToolButton*>(QStringLiteral("snapshotButton"));
    ASSERT_NE(freezeButton, nullptr);
    ASSERT_NE(snapshotButton, nullptr);
    EXPECT_FALSE(freezeButton->isHidden());
    EXPECT_FALSE(snapshotButton->isHidden());
    EXPECT_EQ(freezeButton->defaultAction(), freeze);
    EXPECT_EQ(snapshotButton->defaultAction(), snapshot);

    ASSERT_TRUE(panelOf(*window)->selectImage(1));
    EXPECT_FALSE(freeze->isEnabled());
    EXPECT_FALSE(snapshot->isEnabled());
    EXPECT_TRUE(freezeButton->isHidden());
    EXPECT_TRUE(snapshotButton->isHidden());
}

TEST(MainWindowTest, SnapshotAddsTheShownFrameToTheHistory) {
    const auto window = makeTestWindow();
    const auto receivedAt = std::chrono::system_clock::now();
    showCamera(*window, 5, receivedAt);
    const qint64 milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(receivedAt.time_since_epoch()).count();
    const QString name = snapshotName(QStringLiteral("camera"), milliseconds);

    action(*window, "actionSnapshot")->trigger();

    ASSERT_EQ(panelOf(*window)->listedIds(), std::vector<ImageId>{1});
    EXPECT_EQ(listedName(*window, 1), name);
    EXPECT_EQ(copiedPicture(*window, 1).pixelColor(0, 0).red(), 5);
    const LogEntry* logged = findEntry(*logOf(*window), QStringLiteral("Snapshot [%1] (40×30").arg(name));
    ASSERT_NE(logged, nullptr);
    EXPECT_EQ(logged->kind, LogKind::Info);
}

// (Q1) The snapshot is listed; the stream stays in view.
TEST(MainWindowTest, SnapshotKeepsTheChannelShown) {
    const auto window = makeTestWindow();
    showCamera(*window, 5);

    action(*window, "actionSnapshot")->trigger();

    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));
    EXPECT_EQ(window->shownImageId(), std::nullopt);
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::nullopt);
}

// The snapshot looks as the frame did on screen.
TEST(MainWindowTest, SnapshotKeepsTheChannelsSettings) {
    const auto window = makeTestWindow();
    showCamera(*window, 5);
    imageSubmenu(*window, QStringLiteral("Colours"))->actions().at(1)->trigger(); // Turbo

    action(*window, "actionSnapshot")->trigger();
    ASSERT_TRUE(panelOf(*window)->selectImage(1));

    EXPECT_EQ(buttonText(*window, "coloursButton"), QStringLiteral("Colours: Turbo ▾"));
}

// A frame whose every pixel is its column number, so a snapshot's first pixel tells where it was cut.
cv::Mat columnsFrame() {
    cv::Mat frame(200, 256, CV_8UC1);
    for (int x = 0; x < frame.cols; ++x) {
        frame.col(x).setTo(x);
    }
    return frame;
}

QSize listedImageSize(MainWindow& window, ImageId id) {
    const QAbstractItemModel* model = panelOf(window)->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(ThumbnailModel::IdRole).value<quint64>() == id) {
            return index.data(ThumbnailModel::ImageSizeRole).toSize();
        }
    }
    return {};
}

// Zoomed in, a snapshot is what is in view: those pixels of the original, at full resolution.
TEST(MainWindowTest, SnapshotWhileZoomedInTakesTheAreaInView) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", columnsFrame());
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();
    zoomIn(*view, QRectF(view->rect()).center());
    zoomIn(*view, QRectF(view->rect()).center());
    const QRect area = view->visibleArea();
    ASSERT_LT(area.width(), 256);
    ASSERT_GT(area.x(), 0);

    action(*window, "actionSnapshot")->trigger();

    ASSERT_EQ(panelOf(*window)->listedIds(), std::vector<ImageId>{1});
    EXPECT_EQ(listedImageSize(*window, 1), area.size());
    EXPECT_EQ(copiedPicture(*window, 1).pixelColor(0, 0).red(), area.x());
    const LogEntry* logged = findEntry(
        *logOf(*window), QStringLiteral("cut from the frame at %1, %2").arg(area.x()).arg(area.y()));
    ASSERT_NE(logged, nullptr);
    EXPECT_TRUE(logged->text.contains(QStringLiteral("(%1×%2").arg(area.width()).arg(area.height())))
        << logged->text.toStdString();
}

TEST(MainWindowTest, SnapshotOfAFittedFrameTakesItWhole) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", columnsFrame());
    QApplication::processEvents();

    action(*window, "actionSnapshot")->trigger();

    EXPECT_EQ(listedImageSize(*window, 1), QSize(256, 200));
    EXPECT_EQ(findEntry(*logOf(*window), QStringLiteral("cut from the frame")), nullptr);
}

// The shapes over the frame stay over the same pixels of the cut.
TEST(MainWindowTest, SnapshotWhileZoomedInMovesTheOverlaysWithTheCut) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", columnsFrame(), std::chrono::system_clock::now(), std::nullopt,
             {.overlays = {visual_debug::Point{{128, 100}, "centre"}}});
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();
    zoomIn(*view, QRectF(view->rect()).center());
    const QRect area = view->visibleArea();
    ASSERT_LT(area.width(), 256);

    action(*window, "actionSnapshot")->trigger();
    ASSERT_TRUE(panelOf(*window)->selectImage(1));

    ASSERT_EQ(view->overlays().size(), 1U);
    const auto& point = std::get<viewer_core::Point>(view->overlays()[0]);
    EXPECT_EQ(point.at, cv::Point2d(128 - area.x(), 100 - area.y()));
    EXPECT_EQ(point.label, "centre");
}

// The snapshot's picture is made on a thread of its own: the stream goes on meanwhile, and it is listed when
// ready.
TEST(MainWindowTest, ThreadedSnapshotLeavesTheStreamRunning) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setSnapshotConversionForTests([&latch](const SnapshotJob& job) {
        latch.hold();
        return viewer_core::historyDisplay(job.original, job.origin.tags);
    });
    showSized(window);
    feedLive(window, "camera", liveFrame(50));
    ASSERT_TRUE(QTest::qWaitFor([&window] { return viewedGrey(window) == 50; }));

    action(window, "actionSnapshot")->trigger();
    ASSERT_TRUE(latch.waitUntilEntered());
    EXPECT_TRUE(panelOf(window)->listedIds().empty());
    feedLive(window, "camera", liveFrame(60));
    EXPECT_TRUE(QTest::qWaitFor([&window] { return viewedGrey(window) == 60; }));
    latch.release();

    ASSERT_TRUE(QTest::qWaitFor([&window] { return panelOf(window)->listedIds().size() == 1; }));
    EXPECT_EQ(copiedPicture(window, 1).pixelColor(0, 0).red(), 50); // the frame in view when it was taken
}

// A snapshot reaches the history a moment after it was taken: if a still took the view meanwhile, the
// snapshot is listed without taking it.
TEST(MainWindowTest, ASnapshotArrivingLaterNeverTakesTheView) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setSnapshotConversionForTests([&latch](const SnapshotJob& job) {
        latch.hold();
        return viewer_core::historyDisplay(job.original, job.origin.tags);
    });
    showSized(window);
    window.addImage("still", cv::Mat(40, 40, CV_8UC1, cv::Scalar(100)));
    feedLive(window, "camera", liveFrame(50));
    QApplication::processEvents();
    chooseLiveRow(window, QStringLiteral("camera"));

    action(window, "actionSnapshot")->trigger();
    ASSERT_TRUE(latch.waitUntilEntered());
    ASSERT_TRUE(panelOf(window)->selectImage(1)); // the still takes the view
    latch.release();
    ASSERT_TRUE(QTest::qWaitFor([&window] { return panelOf(window)->listedIds().size() == 2; }));

    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(window)->currentImageId(), std::optional<ImageId>(1));
}

// --- Image tags
// ---------------------------------------------------------------------------------------------

const visual_debug::ImageDescription kHlgDescription{.valueRange = {0, 65535},
                                                     .transfer = visual_debug::Transfer::Hlg,
                                                     .primaries = visual_debug::Primaries::Bt2020};
const viewer_core::ImageTags kHlgTags{.valueRange = {0, 65535},
                                      .transfer = viewer_core::Transfer::Hlg,
                                      .primaries = viewer_core::Primaries::Bt2020};

// Hands a still to the window's sink as the server's thread would, then lets the window take it.
void feedStill(MainWindow& window, const std::string& name, const cv::Mat& image,
               const visual_debug::ImageDescription& description) {
    window.sinkForTests().onImageReceived({.name = name,
                                           .image = image,
                                           .receivedAt = std::chrono::system_clock::now(),
                                           .description = description});
    QApplication::processEvents();
}

QString expectedSize(std::uint64_t bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeSIFormat);
}

// 16384 of 0-65535 is a quarter: grey 64. Without the hint, a constant image is black.
TEST(MainWindowTest, ReceivedHintSetsTheAutoRange) {
    const auto window = makeTestWindow();
    showSized(*window);

    feedStill(*window, "depth", cv::Mat(40, 40, CV_16UC1, cv::Scalar(16384)), {.valueRange = {0, 65535}});

    EXPECT_EQ(viewedGrey(*window), 64);
    EXPECT_EQ(thumbnailGrey(*window, 1), 64);
}

TEST(MainWindowTest, InfoStripNamesTheTags) {
    const auto window = makeTestWindow();

    feedStill(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)), kHlgDescription);

    EXPECT_EQ(infoDetails(*window), QString::fromUtf8("40×30 · 3 ch · 16U · HLG · BT.2020 · 0–65535 · ") +
                                        expectedSize(40 * 30 * 3 * 2));
}

TEST(MainWindowTest, LiveFrameKeepsItsTags) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;

    feedLive(window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)),
             std::chrono::system_clock::now(), std::nullopt, kHlgDescription);
    QApplication::processEvents();
    window.tick(clocked.now);

    EXPECT_TRUE(
        infoDetails(window).contains(QString::fromUtf8("40×30 · 3 ch · 16U · HLG · BT.2020 · 0–65535 · ")))
        << infoDetails(window).toStdString();
}

TEST(MainWindowTest, SnapshotKeepsTheTags) {
    const auto window = makeTestWindow();
    feedLive(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)),
             std::chrono::system_clock::now(), std::nullopt, kHlgDescription);
    QApplication::processEvents();

    action(*window, "actionSnapshot")->trigger();
    ASSERT_TRUE(panelOf(*window)->selectImage(1));

    EXPECT_TRUE(infoDetails(*window).contains(QStringLiteral("HLG · BT.2020")))
        << infoDetails(*window).toStdString();
}

TEST(MainWindowTest, KeptImagesKeepTheirTags) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)), {.tags = kHlgTags});
        ASSERT_TRUE(waitForStoredFiles(root.path(), 1));
    }

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(panelOf(*window)->listedIds().size(), 1U);
    ASSERT_TRUE(panelOf(*window)->selectImage(panelOf(*window)->listedIds().front()));

    EXPECT_TRUE(infoDetails(*window).contains(QStringLiteral("HLG · BT.2020")))
        << infoDetails(*window).toStdString();
}

QAction* rangeEntry(MainWindow& window, const QString& title) {
    return entryTitled(imageSubmenu(window, QStringLiteral("Range")), title);
}

TEST(MainWindowTest, HlgStillShowsInHdrByDefault) {
    const auto window = makeTestWindow();

    feedStill(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)), kHlgDescription);

    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: HDR ▾"));
    EXPECT_TRUE(rangeEntry(*window, QStringLiteral("HDR"))->isVisible());
}

// Auto chosen by hand on an HDR image is kept for the name, though Auto is not its default.
TEST(MainWindowTest, ExplicitAutoOnAnHdrNameIsKept) {
    const auto window = makeTestWindow();
    feedStill(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)), kHlgDescription);

    rangeEntry(*window, QStringLiteral("Auto"))->trigger();
    feedStill(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(8192)), kHlgDescription);

    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: Auto ▾"));
}

// An untagged image of a name set to HDR cannot be tone-mapped: it is shown, and named, as Auto.
TEST(MainWindowTest, HdrRangeOnlyForTaggedImages) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedStill(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)), kHlgDescription);
    rangeEntry(*window, QStringLiteral("HDR"))->trigger();

    feedStill(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(16384)),
              {.valueRange = {0, 65535}});

    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: Auto ▾"));
    EXPECT_FALSE(rangeEntry(*window, QStringLiteral("HDR"))->isVisible());
    EXPECT_EQ(viewedGrey(*window), 64); // Auto with the hint
}

TEST(MainWindowTest, HlgLiveFrameShowsInHdrByDefault) {
    const auto window = makeTestWindow();

    feedLive(*window, "phone", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(4096)),
             std::chrono::system_clock::now(), std::nullopt, kHlgDescription);
    QApplication::processEvents();

    ASSERT_EQ(window->shownLiveChannel(), std::optional<std::string>("phone"));
    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: HDR ▾"));
}

// --- Clipping ---------------------------------------------------------------------------------------------

QColor viewedColour(MainWindow& window) {
    const QImage view = window.findChild<ImageView*>()->grab().toImage();
    return view.pixelColor(view.width() / 2, view.height() / 2);
}

QColor thumbnailColour(MainWindow& window, ImageId id) {
    const QImage thumbnail =
        panelOf(window)->findChild<ThumbnailModel*>()->thumbnail(id, QSize(16, 16)).toImage();
    return thumbnail.pixelColor(thumbnail.width() / 2, thumbnail.height() / 2);
}

// White in 8 bits is at the top of Auto's 0-255, so clipped: magenta in the view, white everywhere else.
TEST(MainWindowTest, ShowClippingMarksTheViewOnly) {
    const auto window = makeTestWindow();
    showSized(*window);
    window->addImage("white", cv::Mat(40, 40, CV_8UC1, cv::Scalar(255)));
    QAction* showClipping = action(*window, "actionShowClipping");
    ASSERT_NE(showClipping, nullptr);
    EXPECT_TRUE(showClipping->isCheckable());
    EXPECT_EQ(showClipping->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    EXPECT_TRUE(displayControlOf(*window)->rangeMenu()->actions().contains(showClipping));

    showClipping->trigger();
    QApplication::processEvents();

    EXPECT_EQ(viewedColour(*window), QColor(255, 0, 255));
    EXPECT_EQ(thumbnailColour(*window, 1), QColor(255, 255, 255));
    EXPECT_EQ(copiedPicture(*window, 1).pixelColor(20, 20), QColor(255, 255, 255));
    showClipping->trigger();
    QApplication::processEvents();
    EXPECT_EQ(viewedColour(*window), QColor(255, 255, 255));
}

TEST(MainWindowTest, ShowClippingMarksTheLiveFrame) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", liveFrame(255));
    QApplication::processEvents();

    action(*window, "actionShowClipping")->trigger();
    QApplication::processEvents();

    EXPECT_EQ(viewedColour(*window), QColor(255, 0, 255));
}

// --- Region statistics ------------------------------------------------------------------------------------

void sendMouse(QWidget& widget, QEvent::Type type, QPointF position, Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(type, position, widget.mapToGlobal(position), Qt::LeftButton, Qt::LeftButton,
                      modifiers);
    QCoreApplication::sendEvent(&widget, &event);
}

// Shift-drags across the whole view, so the region is the whole image.
ImageView* selectWholeImage(MainWindow& window) {
    auto* view = window.findChild<ImageView*>();
    const QPointF corner(view->width() - 1, view->height() - 1);
    sendMouse(*view, QEvent::MouseButtonPress, {0, 0}, Qt::ShiftModifier);
    sendMouse(*view, QEvent::MouseMove, corner, Qt::ShiftModifier);
    sendMouse(*view, QEvent::MouseButtonRelease, corner, Qt::ShiftModifier);
    return view;
}

cv::Mat countingImage() {
    cv::Mat image(4, 4, CV_8UC1);
    for (int i = 0; i < 16; ++i) {
        image.at<uchar>(i / 4, i % 4) = static_cast<uchar>(i);
    }
    return image;
}

TEST(MainWindowTest, RegionShowsStatistics) {
    const auto window = makeTestWindow();
    showSized(*window);
    window->addImage("counting", countingImage());

    const ImageView* view = selectWholeImage(*window);

    EXPECT_EQ(view->regionText(), (QStringList{QString::fromUtf8("4×4 at 0, 0"),
                                               QString::fromUtf8("I  min 0 · max 15 · mean 7.5 · σ 4.61")}));
}

TEST(MainWindowTest, RegionNamesEveryChannelAndWhatIsLeftOut) {
    const auto window = makeTestWindow();
    showSized(*window);
    cv::Mat image(2, 2, CV_32FC3, cv::Scalar(0.25, 1.5, 1000));
    image.at<cv::Vec3f>(0, 0)[1] = std::numeric_limits<float>::quiet_NaN();
    window->addImage("floats", image);

    const ImageView* view = selectWholeImage(*window);

    EXPECT_EQ(view->regionText(), (QStringList{QString::fromUtf8("2×2 at 0, 0"),
                                               QString::fromUtf8("B  min 0.25 · max 0.25 · mean 0.25 · σ 0"),
                                               QString::fromUtf8("G  min 1.5 · max 1.5 · mean 1.5 · σ 0"),
                                               QString::fromUtf8("R  min 1000 · max 1000 · mean 1000 · σ 0"),
                                               QStringLiteral("1 not finite")}));
}

TEST(MainWindowTest, RegionFollowsTheShownImage) {
    const auto window = makeTestWindow();
    showSized(*window);
    window->addImage("ten", cv::Mat(4, 4, CV_8UC1, cv::Scalar(10)));
    window->addImage("twenty", cv::Mat(4, 4, CV_8UC1, cv::Scalar(20)));
    const ImageView* view = selectWholeImage(*window);
    ASSERT_TRUE(view->regionText().value(1).contains(QStringLiteral("mean 20")));

    ASSERT_TRUE(panelOf(*window)->selectImage(1));

    EXPECT_TRUE(view->regionText().value(1).contains(QStringLiteral("mean 10")))
        << view->regionText().join(u'|').toStdString();
}

TEST(MainWindowTest, RegionFollowsTheLiveFrame) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    showSized(window);
    feedLive(window, "camera", liveFrame(10));
    QApplication::processEvents();
    const ImageView* view = selectWholeImage(window);
    ASSERT_TRUE(view->regionText().value(1).contains(QStringLiteral("mean 10")));

    feedLive(window, "camera", liveFrame(20));
    QApplication::processEvents();
    window.tick(clocked.now);

    EXPECT_TRUE(view->regionText().value(1).contains(QStringLiteral("mean 20")))
        << view->regionText().join(u'|').toStdString();
}

TEST(MainWindowTest, EscapeClearsTheRegion) {
    const auto window = makeTestWindow();
    showActive(*window);
    window->addImage("counting", countingImage());
    ImageView* view = selectWholeImage(*window);
    ASSERT_TRUE(view->region().has_value());

    QTest::keyClick(QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : window.get(),
                    Qt::Key_Escape);

    EXPECT_EQ(view->region(), std::nullopt);
    EXPECT_TRUE(view->regionText().isEmpty());
}

TEST(MainWindowTest, FreezeStopsRedrawsWhileCounting) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    showSized(window);
    showCamera(window, 10);
    QAction* freeze = action(window, "actionFreeze");

    freeze->trigger();
    feedLive(window, "camera", liveFrame(20));
    feedLive(window, "camera", liveFrame(30));
    QApplication::processEvents();
    window.tick(clocked.now);

    EXPECT_TRUE(freeze->isChecked());
    EXPECT_EQ(viewedGrey(window), 10);
    EXPECT_TRUE(infoDetails(window).startsWith(QStringLiteral("frozen · ")))
        << infoDetails(window).toStdString();
    EXPECT_TRUE(infoDetails(window).contains(QStringLiteral("1 dropped")))
        << infoDetails(window).toStdString();
    EXPECT_EQ(
        liveRowOf(window, QStringLiteral("camera"))->findChild<QLabel*>(QStringLiteral("liveState"))->text(),
        QStringLiteral("frozen"));

    freeze->trigger();

    EXPECT_FALSE(freeze->isChecked());
    EXPECT_EQ(viewedGrey(window), 30);
}

TEST(MainWindowTest, SpaceFreezesFromTheThumbnails) {
    const auto window = makeTestWindow();
    showActive(*window);
    window->addImage("a", makeImage());
    showCamera(*window, 1);
    panelOf(*window)->setFocus();

    QTest::keyClick(panelOf(*window), Qt::Key_Space);
    EXPECT_TRUE(action(*window, "actionFreeze")->isChecked());
    QTest::keyClick(panelOf(*window), Qt::Key_Space);
    EXPECT_FALSE(action(*window, "actionFreeze")->isChecked());
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));
}

TEST(MainWindowTest, SpaceTypesInTheSearchField) {
    const auto window = makeTestWindow();
    showActive(*window);
    showCamera(*window, 1);
    QLineEdit* search = sidebarOf(*window)->searchField();
    search->setFocus();

    QTest::keyClicks(search, QStringLiteral("a b"));

    EXPECT_EQ(search->text(), QStringLiteral("a b"));
    EXPECT_FALSE(action(*window, "actionFreeze")->isChecked());
}

// The view never takes the focus, so after a pan the arrow keys still step through the thumbnails.
TEST(MainWindowTest, ArrowKeysStillStepAfterPanning) {
    const auto window = makeTestWindow();
    showActive(*window);
    window->addImage("a", makeImage());
    window->addImage("b", makeImage());
    panelOf(*window)->setFocus();
    showCamera(*window, 1);
    auto* view = window->findChild<ImageView*>();
    const QPointF centre = QRectF(view->rect()).center();

    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QTest::mouseMove(view, (centre + QPointF(30, 20)).toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, (centre + QPointF(30, 20)).toPoint());
    ASSERT_TRUE(panelOf(*window)->hasFocus());

    QTest::keyClick(panelOf(*window), Qt::Key_Down);

    EXPECT_TRUE(window->shownImageId().has_value());
    EXPECT_EQ(window->shownLiveChannel(), std::nullopt);
}

// (R3) Colours are offered while the frame in view has one channel, frame by frame.
TEST(MainWindowTest, ColoursFollowTheFramesChannelCount) {
    const auto window = makeTestWindow();
    feedLive(*window, "camera", liveFrame(1));
    QApplication::processEvents();
    const auto* colours = displayControlOf(*window)->findChild<QToolButton*>(QStringLiteral("coloursButton"));
    ASSERT_FALSE(colours->isHidden());

    feedLive(*window, "camera", cv::Mat(30, 40, CV_8UC3, cv::Scalar(1, 2, 3)));
    QApplication::processEvents();
    EXPECT_TRUE(colours->isHidden());

    feedLive(*window, "camera", liveFrame(2));
    QApplication::processEvents();
    EXPECT_FALSE(colours->isHidden());
}

// (F5) A snapshot's thumbnail looks as the channel did when it was taken.
TEST(MainWindowTest, SnapshotThumbnailHasTheChannelsColours) {
    const auto window = makeTestWindow();
    feedLive(*window, "edges", ramp());
    QApplication::processEvents();
    ASSERT_EQ(window->shownLiveChannel(), std::optional<std::string>("edges"));
    chooseColours(*window, 1);

    action(*window, "actionSnapshot")->trigger();

    EXPECT_TRUE(thumbnailIsColoured(*window, 1));
}

// --- Live frames at the size they are drawn (F10) ---------------------------------------------------------

cv::Mat fourK() { return cv::Mat(3000, 4000, CV_8UC3, cv::Scalar(10, 20, 30)); }

// A frame larger than the view is converted at the size it is drawn, not in full; the readout still names the
// frame's own pixels.
TEST(MainWindowTest, LiveFrameIsConvertedAtTheDrawnSize) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();

    const QSize picture = view->picture().size();
    EXPECT_LT(picture.width(), 4000);
    EXPECT_LE(picture.width(), static_cast<int>(std::ceil(view->width() * view->devicePixelRatioF())) + 1);
    QSignalSpy hovered(view, &ImageView::pixelHovered);
    moveMouseOver(*view, QRectF(view->rect()).center());
    ASSERT_EQ(hovered.size(), 1);
    EXPECT_NEAR(hovered.last().at(0).toInt(), 2000, 20);
    EXPECT_NEAR(hovered.last().at(1).toInt(), 1500, 20);
}

// Copy Image and Snapshot take the whole frame, whatever size it is drawn at.
TEST(MainWindowTest, CopyAndSnapshotTakeTheWholeFrame) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();
    QGuiApplication::clipboard()->clear();

    action(*window, "actionCopyImage")->trigger();
    action(*window, "actionSnapshot")->trigger();

    EXPECT_EQ(QGuiApplication::clipboard()->image().size(), QSize(4000, 3000));
    const auto* model = panelOf(*window)->findChild<ThumbnailModel*>();
    ASSERT_EQ(model->rowCount(), 1);
    EXPECT_EQ(model->index(0).data(ThumbnailModel::ImageSizeRole).toSize(), QSize(4000, 3000));
}

// Zooms in at the view's centre until a frame of `frameSize` is drawn at 1:1 or closer.
void zoomPastOneToOne(ImageView& view, QSize frameSize) {
    for (int i = 0; i < 20 && view.deviceScaleFor(frameSize, ImageView::KeepView::IfSameSize) < 1.0; ++i) {
        zoomIn(view, QRectF(view.rect()).center());
    }
}

// Zoomed in to 1:1 or closer, the frame's pixels in view are converted at full size, so every pixel shows.
TEST(MainWindowTest, ZoomedToOneToOneDrawsEveryPixelInView) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();
    action(*window, "actionFreeze")->trigger();
    auto* view = window->findChild<ImageView*>();

    zoomPastOneToOne(*view, QSize(4000, 3000));

    EXPECT_TRUE(view->pictureArea().contains(view->visibleArea()));
    EXPECT_EQ(view->picture().size(), view->pictureArea().size());
}

// Zoomed in, only the part of the frame in view is converted, with a quarter of the view's width and height
// around it, so a pan within that shows pixels at once; fitted, the whole frame.
TEST(MainWindowTest, ZoomedInConvertsOnlyTheAreaInView) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();
    ASSERT_EQ(view->pictureArea(), QRect(0, 0, 4000, 3000));

    zoomIn(*view, QRectF(view->rect()).center());
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();

    const QRect visible = view->visibleArea();
    const QRect area = view->pictureArea();
    EXPECT_TRUE(area.contains(visible));
    EXPECT_NEAR(area.left(), visible.left() - visible.width() / 4, 2);
    EXPECT_NEAR(area.right(), visible.right() + visible.width() / 4, 2);
    EXPECT_NEAR(area.top(), visible.top() - visible.height() / 4, 2);
    EXPECT_NEAR(area.bottom(), visible.bottom() + visible.height() / 4, 2);
    // Below 1:1, at the size it is drawn.
    const double scale = view->deviceScaleFor(QSize(4000, 3000), ImageView::KeepView::IfSameSize);
    ASSERT_LT(scale, 1.0);
    EXPECT_NEAR(view->picture().width(), area.width() * scale, 2);

    sendMouse(*view, QEvent::MouseButtonDblClick, QRectF(view->rect()).center(), Qt::NoModifier);
    EXPECT_EQ(view->pictureArea(), QRect(0, 0, 4000, 3000));
}

// A left-button drag in the view from `from` to `to`: a pan.
void pan(QWidget& view, QPointF from, QPointF to) {
    sendMouse(view, QEvent::MouseButtonPress, from, Qt::NoModifier);
    sendMouse(view, QEvent::MouseMove, to, Qt::NoModifier);
    sendMouse(view, QEvent::MouseButtonRelease, to, Qt::NoModifier);
}

// A pan within the converted area's margin converts nothing; past it, even a frozen frame is converted again
// for what is now in view.
TEST(MainWindowTest, PanningBeyondTheCutConvertsAgain) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();
    action(*window, "actionFreeze")->trigger();
    auto* view = window->findChild<ImageView*>();
    const QPointF centre = QRectF(view->rect()).center();
    zoomIn(*view, centre);
    const qint64 converted = view->picture().cacheKey();
    ASSERT_NE(view->pictureArea(), QRect(0, 0, 4000, 3000));

    pan(*view, centre, centre - QPointF(10, 0));
    EXPECT_EQ(view->picture().cacheKey(), converted);

    pan(*view, centre, centre - QPointF(view->width() / 2.0, 0));
    EXPECT_NE(view->picture().cacheKey(), converted);
    EXPECT_TRUE(view->pictureArea().contains(view->visibleArea()));
}

// The part converted is stretched with the whole frame's range under Auto, so zooming in does not brighten a
// dim area: its value 1000 is grey 4 of a frame from 0 to 65535, not black or white as the part's own range.
TEST(MainWindowTest, ZoomedInLooksAsTheWholeFrameWould) {
    const auto window = makeTestWindow();
    showSized(*window);
    cv::Mat frame(3000, 4000, CV_16UC1, cv::Scalar(1000));
    frame.at<std::uint16_t>(0, 0) = 0;
    frame.at<std::uint16_t>(2999, 3999) = 65535;
    feedLive(*window, "camera", frame);
    QApplication::processEvents();
    auto* view = window->findChild<ImageView*>();

    zoomIn(*view, QRectF(view->rect()).center());
    feedLive(*window, "camera", frame);
    QApplication::processEvents();

    ASSERT_FALSE(view->pictureArea().contains(QRect(0, 0, 1, 1)));
    EXPECT_EQ(viewedGrey(*window), 4);
}

// Past 1:1 the picture holds every pixel of its area already, so zooming further, or a resize, converts
// nothing again: each step of a trackpad zoom would otherwise convert the area again.
TEST(MainWindowTest, ZoomingPastOneToOneConvertsNothingAgain) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", fourK());
    QApplication::processEvents();
    action(*window, "actionFreeze")->trigger();
    auto* view = window->findChild<ImageView*>();
    zoomPastOneToOne(*view, QSize(4000, 3000));
    ASSERT_EQ(view->picture().size(), view->pictureArea().size());
    const qint64 converted = view->picture().cacheKey();

    zoomIn(*view, QRectF(view->rect()).center());
    window->resize(window->width() + 40, window->height());
    QApplication::processEvents();

    EXPECT_EQ(view->picture().cacheKey(), converted);
}

// A resize redraws even a frozen frame at its new size, without counting it as a frame drawn.
TEST(MainWindowTest, ResizingRedrawsAFrozenFrame) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    showSized(window);
    feedLive(window, "camera", fourK());
    QApplication::processEvents();
    action(window, "actionFreeze")->trigger();
    window.tick(clocked.now);
    auto* view = window.findChild<ImageView*>();
    const QSize before = view->picture().size();
    const QString details = infoDetails(window);

    window.resize(1600, 1200);
    QApplication::processEvents();
    window.tick(clocked.now);

    EXPECT_GT(view->picture().size().width(), before.width());
    EXPECT_EQ(infoDetails(window), details); // the same frames drawn
}

// --- The arrow keys across live rows and history (F8) -----------------------------------------------------

// Channels camera and edges, and stills a (1) then b (2), so b is listed first; camera in view and the
// keyboard on the thumbnails.
std::unique_ptr<MainWindow> windowWithChannelsAndStills() {
    auto window = makeTestWindow();
    showActive(*window);
    window->addImage("a", makeImage());
    window->addImage("b", makeImage());
    feedLive(*window, "camera", liveFrame(1));
    feedLive(*window, "edges", liveFrame(2));
    QApplication::processEvents();
    chooseLiveRow(*window, QStringLiteral("camera"));
    panelOf(*window)->setFocus();
    return window;
}

void press(MainWindow& window, Qt::Key key) { QTest::keyClick(panelOf(window), key); }

// The live rows, then the thumbnails as listed, are one list; nothing happens past either end.
TEST(MainWindowTest, ArrowKeysMoveThroughLiveRowsAndHistory) {
    const auto window = windowWithChannelsAndStills();
    const auto shown = [&window]() -> std::string {
        if (const auto channel = window->shownLiveChannel()) {
            return *channel;
        }
        return window->shownImageId() ? "image " + std::to_string(*window->shownImageId()) : "nothing";
    };
    ASSERT_EQ(shown(), "camera");

    press(*window, Qt::Key_Down);
    EXPECT_EQ(shown(), "edges");
    EXPECT_EQ(livePanelOf(*window)->selectedChannel(), QStringLiteral("edges"));
    press(*window, Qt::Key_Down);
    EXPECT_EQ(shown(), "image 2");
    press(*window, Qt::Key_Down);
    EXPECT_EQ(shown(), "image 1");
    press(*window, Qt::Key_Down);
    EXPECT_EQ(shown(), "image 1");
    press(*window, Qt::Key_Up);
    EXPECT_EQ(shown(), "image 2");
    press(*window, Qt::Key_Up);
    EXPECT_EQ(shown(), "edges");
    EXPECT_EQ(panelOf(*window)->currentImageId(), std::nullopt);
    press(*window, Qt::Key_Up);
    EXPECT_EQ(shown(), "camera");
    press(*window, Qt::Key_Up);
    EXPECT_EQ(shown(), "camera");
    EXPECT_TRUE(panelOf(*window)->hasFocus());
}

TEST(MainWindowTest, ArrowKeysSkipMutedRows) {
    const auto window = windowWithChannelsAndStills();
    clickMute(*window, QStringLiteral("edges"));

    press(*window, Qt::Key_Down);
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(2));
    press(*window, Qt::Key_Up);
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));
}

// With no history the keys still move among the channels, and from an empty view Down goes to the first.
TEST(MainWindowTest, ArrowKeysWorkWithAnEmptyHistory) {
    const auto window = makeTestWindow();
    showActive(*window);
    feedLive(*window, "camera", liveFrame(1));
    feedLive(*window, "edges", liveFrame(2));
    QApplication::processEvents();
    panelOf(*window)->setFocus();
    ASSERT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));

    press(*window, Qt::Key_Down);
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("edges"));
    press(*window, Qt::Key_Down);
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("edges"));
    press(*window, Qt::Key_Up);
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("camera"));

    clickMute(*window, QStringLiteral("camera")); // the view is empty now
    ASSERT_EQ(window->shownLiveChannel(), std::nullopt);
    press(*window, Qt::Key_Down);
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("edges"));
}

// The keys follow what the sidebar lists: here the search hides b.
TEST(MainWindowTest, ArrowKeysFollowTheListedOrder) {
    const auto window = windowWithChannelsAndStills();
    chooseLiveRow(*window, QStringLiteral("edges"));
    sidebarOf(*window)->searchField()->setText(QStringLiteral("a"));
    panelOf(*window)->setFocus();

    press(*window, Qt::Key_Down);
    EXPECT_EQ(window->shownImageId(), std::optional<ImageId>(1));
    press(*window, Qt::Key_Up);
    EXPECT_EQ(window->shownLiveChannel(), std::optional<std::string>("edges"));
}

// The Live group shows three rows; a key that moves to a fourth or fifth scrolls it into sight.
TEST(MainWindowTest, ArrowKeysScrollTheLiveRowsIntoView) {
    const auto window = makeTestWindow();
    showActive(*window);
    for (int i = 1; i <= 5; ++i) {
        feedLive(*window, "c" + std::to_string(i), liveFrame(i));
    }
    QApplication::processEvents();
    panelOf(*window)->setFocus();
    ASSERT_EQ(window->shownLiveChannel(), std::optional<std::string>("c1"));
    // The rows' layout settles through posted events, long before a person presses a key.
    auto* scroll = livePanelOf(*window)->findChild<QScrollArea*>();
    ASSERT_TRUE(QTest::qWaitFor([scroll] { return scroll->verticalScrollBar()->maximum() > 0; }));

    for (int i = 0; i < 4; ++i) {
        press(*window, Qt::Key_Down);
    }

    ASSERT_EQ(window->shownLiveChannel(), std::optional<std::string>("c5"));
    const QWidget* row = liveRowOf(*window, QStringLiteral("c5"));
    EXPECT_TRUE(scroll->viewport()->rect().contains(
        QRect(row->mapTo(scroll->viewport(), QPoint(0, 0)), row->size())));
}

// (R5) Space presses a button that has the keyboard focus, instead of freezing the channel in view.
TEST(MainWindowTest, SpacePressesAFocusedButton) {
    const auto window = makeTestWindow();
    showActive(*window);
    showCamera(*window, 1);
    auto* button = window->findChild<QToolButton*>(QStringLiteral("logToggle"));
    button->setFocusPolicy(Qt::StrongFocus); // as with macOS keyboard navigation turned on
    button->setFocus();
    QSignalSpy clicked(button, &QAbstractButton::clicked);

    QTest::keyClick(button, Qt::Key_Space);

    EXPECT_EQ(clicked.size(), 1);
    EXPECT_FALSE(action(*window, "actionFreeze")->isChecked());
}

TEST(MainWindowTest, CopyImageTakesTheLiveFrame) {
    const auto window = makeTestWindow();
    showCamera(*window, 77);
    QGuiApplication::clipboard()->clear();

    ASSERT_TRUE(action(*window, "actionCopyImage")->isEnabled());
    action(*window, "actionCopyImage")->trigger();

    const QImage copied = QGuiApplication::clipboard()->image();
    EXPECT_EQ(copied.size(), QSize(40, 30));
    EXPECT_EQ(copied.pixelColor(0, 0).red(), 77);
    EXPECT_NE(findEntry(*logOf(*window), QStringLiteral("Copied [camera] to the clipboard")), nullptr);
}

// Live frames and snapshots stay in memory, even with Keep Images Between Sessions on.
TEST(MainWindowTest, SnapshotsAndLiveFramesAreNeverStored) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        for (int i = 0; i < 5; ++i) {
            feedLive(*window, "camera", liveFrame(i));
            QApplication::processEvents();
        }
        action(*window, "actionSnapshot")->trigger();
        window->duplicateImage(1); // a copy of a snapshot is one too
        window->addImage("still", makeImage());

        ASSERT_TRUE(waitForStoredFiles(root.path(), 1));
        QTest::qWait(200);
        EXPECT_EQ(storedFiles(root.path()), 1);
    }

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(listedNames(*window), std::vector<QString>{QStringLiteral("still")});
}

// --- Mute -------------------------------------------------------------------------------------------------

QString liveStateOf(MainWindow& window, const QString& name) {
    return liveRowOf(window, name)->findChild<QLabel*>(QStringLiteral("liveState"))->text();
}

// The grey at the middle of a row's picture.
int rowPictureGrey(MainWindow& window, const QString& name) {
    const QImage picture =
        liveRowOf(window, name)->findChild<QLabel*>(QStringLiteral("liveThumbnail"))->pixmap().toImage();
    return picture.isNull() ? -1 : picture.pixelColor(picture.width() / 2, picture.height() / 2).red();
}

// (F5) A still and a live channel of one name share its settings: changing them on the still redraws the
// channel's row too, even while no new frame comes.
TEST(MainWindowTest, StillsSettingsRedrawTheLiveRowOfItsName) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    feedLive(window, "camera", liveFrame(100));
    QApplication::processEvents();
    window.addImage("camera", liveFrame(100));
    panelOf(window)->selectImage(1); // the still, leaving the channel
    ASSERT_EQ(rowPictureGrey(window, QStringLiteral("camera")), 100);

    chooseColours(window, 1); // Turbo
    window.tick(clocked.now);

    const QImage picture = liveRowOf(window, QStringLiteral("camera"))
                               ->findChild<QLabel*>(QStringLiteral("liveThumbnail"))
                               ->pixmap()
                               .toImage();
    const QColor middle = picture.pixelColor(picture.width() / 2, picture.height() / 2);
    EXPECT_TRUE(middle.red() != middle.green() || middle.green() != middle.blue());
}

// (F7) A muted channel's frames are still taken and counted, so its sender is never slowed, but nothing is
// drawn or logged for it until it is unmuted.
TEST(MainWindowTest, MutedChannelStopsDrawingAndLogging) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    feedLive(window, "camera", liveFrame(10));
    feedLive(window, "edges", liveFrame(20));
    QApplication::processEvents();
    chooseLiveRow(window, QStringLiteral("edges"));
    window.tick(clocked.now);
    ASSERT_EQ(rowPictureGrey(window, QStringLiteral("camera")), 10);

    clickMute(window, QStringLiteral("camera"));
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(feedLive(window, "camera", liveFrame(100 + i)), visual_debug::Acceptance::Taken);
        feedLive(window, "edges", liveFrame(30 + i));
        QApplication::processEvents();
    }
    window.tick(clocked.now);

    EXPECT_EQ(rowPictureGrey(window, QStringLiteral("camera")), 10);
    EXPECT_FALSE(liveRowOf(window, QStringLiteral("camera"))
                     ->findChild<QLabel*>(QStringLiteral("liveThumbnail"))
                     ->isEnabled());
    EXPECT_EQ(liveStateOf(window, QStringLiteral("camera")), QStringLiteral("muted"));
    EXPECT_TRUE(liveRowOf(window, QStringLiteral("camera"))
                    ->findChild<QLabel*>(QStringLiteral("liveRate"))
                    ->text()
                    .isEmpty());
    const ActivityLog* log = logOf(window);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Muted live channel [camera]")), 1);
    EXPECT_EQ(window.shownLiveChannel(), std::optional<std::string>("edges"));

    clocked.now += viewer_core::kIdleAfter;
    window.tick(clocked.now);
    EXPECT_EQ(countEntries(*log, QStringLiteral("Live channel [camera] idle")), 0);
    EXPECT_EQ(liveStateOf(window, QStringLiteral("camera")), QStringLiteral("muted · idle"));

    clickMute(window, QStringLiteral("camera"));
    EXPECT_EQ(countEntries(*log, QStringLiteral("Unmuted live channel [camera]")), 1);
}

// (F7) Muting the channel in view gives the view back, as closing it does, and unfreezes it.
TEST(MainWindowTest, MutingTheFrozenChannelInViewUnfreezesIt) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    window.addImage("a", makeImage());
    feedLive(window, "camera", liveFrame(1));
    QApplication::processEvents();
    chooseLiveRow(window, QStringLiteral("camera"));
    action(window, "actionFreeze")->trigger();
    ASSERT_TRUE(action(window, "actionFreeze")->isChecked());

    clickMute(window, QStringLiteral("camera"));

    EXPECT_EQ(window.shownLiveChannel(), std::nullopt);
    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(panelOf(window)->currentImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(livePanelOf(window)->selectedChannel(), std::nullopt);
    EXPECT_FALSE(action(window, "actionFreeze")->isChecked());

    clickMute(window, QStringLiteral("camera"));
    window.tick(clocked.now);
    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(liveStateOf(window, QStringLiteral("camera")), QStringLiteral("live"));
}

// (F7) A click on a muted row does nothing, and a muted channel never takes an empty view.
TEST(MainWindowTest, MutedChannelIsNeverShown) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    window.addImage("a", makeImage());
    feedLive(window, "camera", liveFrame(1));
    QApplication::processEvents();
    clickMute(window, QStringLiteral("camera"));

    chooseLiveRow(window, QStringLiteral("camera"));
    EXPECT_EQ(window.shownLiveChannel(), std::nullopt);
    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));

    ClockedWindow empty;
    feedLive(*empty.window, "camera", liveFrame(1));
    QApplication::processEvents();
    ASSERT_EQ(empty.window->shownLiveChannel(), std::optional<std::string>("camera"));
    clickMute(*empty.window, QStringLiteral("camera"));
    EXPECT_EQ(empty.window->shownLiveChannel(), std::nullopt);
    feedLive(*empty.window, "camera", liveFrame(2));
    QApplication::processEvents();
    EXPECT_EQ(empty.window->shownLiveChannel(), std::nullopt);
    EXPECT_FALSE(empty.window->findChild<ImageView*>()->hasImage());
}

// (F7) Mute belongs to the name for the session: closed and back, the channel is still muted, and says
// nothing.
TEST(MainWindowTest, MuteLastsWhenTheChannelComesBack) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    feedLive(window, "camera", liveFrame(1));
    QApplication::processEvents();
    clickMute(window, QStringLiteral("camera"));
    clocked.now += viewer_core::kIdleAfter;
    window.tick(clocked.now);
    liveRowOf(window, QStringLiteral("camera"))
        ->findChild<QToolButton*>(QStringLiteral("liveClose"))
        ->click();
    ASSERT_EQ(liveRowOf(window, QStringLiteral("camera")), nullptr);

    feedLive(window, "camera", liveFrame(2));
    QApplication::processEvents();
    window.tick(clocked.now);

    ASSERT_NE(liveRowOf(window, QStringLiteral("camera")), nullptr);
    EXPECT_EQ(liveStateOf(window, QStringLiteral("camera")), QStringLiteral("muted"));
    EXPECT_TRUE(liveRowOf(window, QStringLiteral("camera"))
                    ->findChild<QToolButton*>(QStringLiteral("liveMute"))
                    ->isChecked());
    EXPECT_EQ(countEntries(*logOf(window), QStringLiteral("Live channel [camera] started")), 1);
    EXPECT_EQ(window.shownLiveChannel(), std::nullopt);
}

// (F7) A muted channel keeps its place among the eight, and once idle can make room like any other.
TEST(MainWindowTest, MutedIdleChannelCanBeReplaced) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    for (int i = 1; i <= 8; ++i) {
        feedLive(window, "cam" + std::to_string(i), liveFrame(i));
    }
    QApplication::processEvents();
    clickMute(window, QStringLiteral("cam1"));
    EXPECT_EQ(feedLive(window, "cam9", liveFrame(9)), visual_debug::Acceptance::TooManyLiveChannels);
    clocked.now += viewer_core::kIdleAfter;

    EXPECT_EQ(feedLive(window, "cam9", liveFrame(9)), visual_debug::Acceptance::Taken);
    QApplication::processEvents();

    EXPECT_NE(findEntry(*logOf(window), QStringLiteral("Live channel [cam9] replaced idle channel [cam1]")),
              nullptr);
}

// --- Overlays ---------------------------------------------------------------------------------------------

viewer_core::ImageTags tagsWithOverlays(int count) {
    std::vector<viewer_core::Overlay> overlays;
    for (int i = 0; i < count; ++i) {
        overlays.push_back(viewer_core::Rectangle{{1.0 + i, 1, 4, 4}, "box"});
    }
    return {.overlays = viewer_core::makeOverlays(std::move(overlays))};
}

visual_debug::ImageDescription descriptionWithOverlays(int count) {
    visual_debug::ImageDescription description;
    for (int i = 0; i < count; ++i) {
        description.overlays.push_back(visual_debug::Rectangle{{1.0 + i, 1, 4, 4}, "box"});
    }
    return description;
}

ImageView* viewOf(MainWindow& window) { return window.findChild<ImageView*>(); }

QToolButton* overlaysButtonOf(MainWindow& window) {
    return infoBarOf(window)->findChild<QToolButton*>(QStringLiteral("overlaysButton"));
}

TEST(MainWindowTest, ShowOverlaysIsACheckedImageMenuAction) {
    const auto window = makeTestWindow();
    QAction* showOverlays = action(*window, "actionShowOverlays");
    ASSERT_NE(showOverlays, nullptr);

    EXPECT_TRUE(showOverlays->isCheckable());
    EXPECT_TRUE(showOverlays->isChecked());
    EXPECT_EQ(showOverlays->shortcuts(),
              (QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+O")), QKeySequence(Qt::Key_O)}));
    EXPECT_FALSE(displayControlOf(*window)->rangeMenu()->actions().contains(showOverlays));
}

TEST(MainWindowTest, ImagesShowTheirOverlays) {
    const auto window = makeTestWindow();

    window->addImage("card", makeImage(), {.tags = tagsWithOverlays(3)});

    EXPECT_EQ(viewOf(*window)->overlays().size(), 3U);
    ASSERT_NE(overlaysButtonOf(*window), nullptr);
    EXPECT_FALSE(overlaysButtonOf(*window)->isHidden());
    EXPECT_EQ(overlaysButtonOf(*window)->text(), QString::fromUtf8("◩ 3 overlays"));
    EXPECT_TRUE(overlaysButtonOf(*window)->isChecked());

    window->addImage("plain", makeImage());

    EXPECT_TRUE(viewOf(*window)->overlays().empty());
    EXPECT_TRUE(overlaysButtonOf(*window)->isHidden());
}

TEST(MainWindowTest, OneOverlayIsSingular) {
    const auto window = makeTestWindow();

    window->addImage("card", makeImage(), {.tags = tagsWithOverlays(1)});

    EXPECT_EQ(overlaysButtonOf(*window)->text(), QString::fromUtf8("◩ 1 overlay"));
}

TEST(MainWindowTest, SwitchHidesOverlaysInTheViewOnly) {
    const auto window = makeTestWindow();
    showSized(*window);
    window->addImage("white", cv::Mat(40, 40, CV_8UC1, cv::Scalar(255)),
                     {.tags = {.overlays = viewer_core::makeOverlays(
                                   {viewer_core::Rectangle{{0, 0, 40, 40}, "", {0, 0, 0xFF}}})}});
    QApplication::processEvents();
    const QColor withOverlays = viewedColour(*window);
    ASSERT_NE(withOverlays, QColor(255, 255, 255));

    overlaysButtonOf(*window)->click();
    QApplication::processEvents();

    EXPECT_FALSE(action(*window, "actionShowOverlays")->isChecked());
    EXPECT_FALSE(viewOf(*window)->overlaysShown());
    EXPECT_EQ(overlaysButtonOf(*window)->text(), QString::fromUtf8("◩ 1 overlay hidden"));
    EXPECT_EQ(viewedColour(*window), QColor(255, 255, 255));
    EXPECT_EQ(copiedPicture(*window, 1).pixelColor(20, 20), QColor(255, 255, 255));
}

TEST(MainWindowTest, CopyNeverHasOverlays) {
    const auto window = makeTestWindow();
    window->addImage("white", cv::Mat(40, 40, CV_8UC1, cv::Scalar(255)), {.tags = tagsWithOverlays(2)});

    EXPECT_EQ(copiedPicture(*window, 1).pixelColor(2, 2), QColor(255, 255, 255));
}

TEST(MainWindowTest, SwitchStaysOffAcrossImages) {
    const auto window = makeTestWindow();
    window->addImage("first", makeImage(), {.tags = tagsWithOverlays(3)});
    action(*window, "actionShowOverlays")->trigger();

    window->addImage("second", makeImage(), {.tags = tagsWithOverlays(1)});

    EXPECT_FALSE(viewOf(*window)->overlaysShown());
    EXPECT_FALSE(overlaysButtonOf(*window)->isChecked());
    EXPECT_EQ(overlaysButtonOf(*window)->text(), QString::fromUtf8("◩ 1 overlay hidden"));
}

TEST(MainWindowTest, OKeyTogglesWithTheThumbnailListFocused) {
    const auto window = makeTestWindow();
    showActive(*window);
    window->addImage("card", makeImage(), {.tags = tagsWithOverlays(1)});
    panelOf(*window)->setFocus();

    QTest::keyClick(panelOf(*window), Qt::Key_O);

    EXPECT_FALSE(action(*window, "actionShowOverlays")->isChecked());
    EXPECT_FALSE(viewOf(*window)->overlaysShown());
}

TEST(MainWindowTest, DisplaySettingsAndClippingKeepOverlays) {
    const auto window = makeTestWindow();
    window->addImage("card", makeImage(), {.tags = tagsWithOverlays(3)});

    action(*window, "actionShowClipping")->trigger();
    EXPECT_EQ(viewOf(*window)->overlays().size(), 3U);
    rangeEntry(*window, QStringLiteral("Hold"))->trigger();

    EXPECT_EQ(viewOf(*window)->overlays().size(), 3U);
    EXPECT_FALSE(overlaysButtonOf(*window)->isHidden());
}

TEST(MainWindowTest, LiveFramesBringTheirOwnOverlays) {
    const auto window = makeTestWindow();

    feedLive(*window, "camera", liveFrame(10), std::chrono::system_clock::now(), std::nullopt,
             descriptionWithOverlays(2));
    QApplication::processEvents();
    EXPECT_EQ(viewOf(*window)->overlays().size(), 2U);
    EXPECT_EQ(overlaysButtonOf(*window)->text(), QString::fromUtf8("◩ 2 overlays"));

    feedLive(*window, "camera", liveFrame(20));
    QApplication::processEvents();
    EXPECT_TRUE(viewOf(*window)->overlays().empty());
    EXPECT_TRUE(overlaysButtonOf(*window)->isHidden());

    feedLive(*window, "camera", liveFrame(30), std::chrono::system_clock::now(), std::nullopt,
             descriptionWithOverlays(1));
    QApplication::processEvents();
    EXPECT_EQ(viewOf(*window)->overlays().size(), 1U);
    EXPECT_FALSE(overlaysButtonOf(*window)->isHidden());
}

TEST(MainWindowTest, FrozenChannelKeepsItsFramesOverlays) {
    const auto window = makeTestWindow();
    feedLive(*window, "camera", liveFrame(10), std::chrono::system_clock::now(), std::nullopt,
             descriptionWithOverlays(2));
    QApplication::processEvents();

    action(*window, "actionFreeze")->trigger();
    feedLive(*window, "camera", liveFrame(20));
    QApplication::processEvents();

    EXPECT_EQ(viewOf(*window)->overlays().size(), 2U);
    action(*window, "actionFreeze")->trigger();
    EXPECT_TRUE(viewOf(*window)->overlays().empty());
}

TEST(MainWindowTest, SnapshotKeepsOverlays) {
    const auto window = makeTestWindow();
    feedLive(*window, "camera", liveFrame(10), std::chrono::system_clock::now(), std::nullopt,
             descriptionWithOverlays(2));
    QApplication::processEvents();

    action(*window, "actionSnapshot")->trigger();
    ASSERT_TRUE(panelOf(*window)->selectImage(1));

    EXPECT_EQ(viewOf(*window)->overlays().size(), 2U);
}

TEST(MainWindowTest, KeptImagesKeepOverlaysAcrossARestart) {
    const QTemporaryDir root;
    setKeepImagesSetting(true);
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        window->addImage("card", makeImage(), {.tags = tagsWithOverlays(4)});
        ASSERT_TRUE(waitForStoredFiles(root.path(), 1));
    }

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(panelOf(*window)->listedIds().size(), 1U);
    ASSERT_TRUE(panelOf(*window)->selectImage(panelOf(*window)->listedIds().front()));

    EXPECT_EQ(viewOf(*window)->overlays().size(), 4U);
}

TEST(MainWindowTest, RenameKeepsOverlays) {
    const auto window = makeTestWindow();
    window->addImage("card", makeImage(), {.tags = tagsWithOverlays(2)});

    window->renameImage(1, QStringLiteral("passport"));

    EXPECT_EQ(viewOf(*window)->overlays().size(), 2U);
}

TEST(MainWindowTest, ShowOverlaysIsOnAtEveryStart) {
    const QTemporaryDir root;
    {
        const auto window = makeRestoredWindow(root.path());
        ASSERT_NE(window, nullptr);
        action(*window, "actionShowOverlays")->trigger();
        ASSERT_FALSE(action(*window, "actionShowOverlays")->isChecked());
    }

    const auto window = makeRestoredWindow(root.path());
    ASSERT_NE(window, nullptr);

    EXPECT_TRUE(action(*window, "actionShowOverlays")->isChecked());
    EXPECT_TRUE(viewOf(*window)->overlaysShown());
}

// --- The histogram strip
// ------------------------------------------------------------------------------------

HistogramStrip* stripOf(MainWindow& window) { return window.findChild<HistogramStrip*>(); }

void showHistogram(MainWindow& window) { action(window, "actionShowHistogram")->trigger(); }

void stripEvent(HistogramStrip& strip, QEvent::Type type, double x, Qt::MouseButton button,
                Qt::MouseButtons buttons) {
    const QPointF position(x, strip.height() / 2.0);
    QMouseEvent event(type, position, strip.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&strip, &event);
}

void pressOnStrip(HistogramStrip& strip, double value) {
    stripEvent(strip, QEvent::MouseButtonPress, strip.xOf(value), Qt::LeftButton, Qt::LeftButton);
}

void moveOnStrip(HistogramStrip& strip, double value) {
    stripEvent(strip, QEvent::MouseMove, strip.xOf(value), Qt::NoButton, Qt::LeftButton);
}

void releaseOnStrip(HistogramStrip& strip, double value) {
    stripEvent(strip, QEvent::MouseButtonRelease, strip.xOf(value), Qt::LeftButton, Qt::NoButton);
}

// From one value to another on the strip's axis, as a hand would: press, move, a turn of the event loop,
// release.
void dragOnStrip(HistogramStrip& strip, double fromValue, double toValue) {
    pressOnStrip(strip, fromValue);
    moveOnStrip(strip, toValue);
    QApplication::processEvents();
    releaseOnStrip(strip, toValue);
    QApplication::processEvents();
}

void doubleClickStrip(HistogramStrip& strip, double value) {
    pressOnStrip(strip, value);
    releaseOnStrip(strip, value);
    pressOnStrip(strip, value);
    stripEvent(strip, QEvent::MouseButtonDblClick, strip.xOf(value), Qt::LeftButton, Qt::LeftButton);
    releaseOnStrip(strip, value);
    QApplication::processEvents();
}

cv::Mat greyStill(int value) { return cv::Mat(40, 40, CV_8UC1, cv::Scalar(value)); }

TEST(MainWindowTest, HistogramStripHiddenInAFreshWindow) {
    const auto window = makeTestWindow();
    showSized(*window);

    EXPECT_FALSE(stripOf(*window)->isVisible());
    showHistogram(*window);
    EXPECT_FALSE(stripOf(*window)->isVisible()); // nothing in view
}

TEST(MainWindowTest, HistogramIsOffAtFirstStartAndRemembered) {
    {
        const auto window = makeTestWindow();
        window->addImage("a", greyStill(10));
        showSized(*window);
        EXPECT_FALSE(stripOf(*window)->isVisible());

        showHistogram(*window);

        EXPECT_TRUE(stripOf(*window)->isVisible());
        EXPECT_TRUE(QSettings().value(QStringLiteral("viewer/histogramShown")).toBool());
    }
    const auto window = makeTestWindow();
    window->addImage("a", greyStill(10));
    showSized(*window);

    EXPECT_TRUE(action(*window, "actionShowHistogram")->isChecked());
    EXPECT_TRUE(stripOf(*window)->isVisible());
}

TEST(MainWindowTest, HistogramHidesWithNothingInView) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);

    window->addImage("a", greyStill(10));
    EXPECT_TRUE(stripOf(*window)->isVisible());
    window->deleteImage(1);
    EXPECT_FALSE(stripOf(*window)->isVisible());
}

TEST(MainWindowTest, HistogramFollowsTheShownStill) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    window->addImage("a", greyStill(10));
    window->addImage("b", greyStill(200));

    ASSERT_EQ(stripOf(*window)->histogram().counts.size(), 1u);
    EXPECT_GT(stripOf(*window)->histogram().counts[0][200], 0u);
    ASSERT_TRUE(panelOf(*window)->selectImage(1));
    ASSERT_EQ(stripOf(*window)->histogram().counts.size(), 1u);
    EXPECT_GT(stripOf(*window)->histogram().counts[0][10], 0u);
    EXPECT_EQ(stripOf(*window)->histogram().counts[0][200], 0u);
}

TEST(MainWindowTest, HistogramFollowsASettingsChange) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    window->addImage("a", greyStill(10));

    applyFixedRange(*window, QStringLiteral("10"), QStringLiteral("20"));

    ASSERT_TRUE(stripOf(*window)->markers());
    EXPECT_EQ(stripOf(*window)->markers()->range, (viewer_core::DisplayRange{10, 20}));
}

TEST(MainWindowTest, HistogramCountsOnlyTheLiveFrameInView) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);

    feedLive(*window, "a", liveFrame(50));
    QApplication::processEvents(); // the first channel takes the view
    feedLive(*window, "a", liveFrame(60));
    feedLive(*window, "b", liveFrame(200));
    QApplication::processEvents();

    const viewer_core::Histogram& histogram = stripOf(*window)->histogram();
    ASSERT_EQ(histogram.counts.size(), 1u);
    EXPECT_GT(histogram.counts[0][60], 0u);
    EXPECT_EQ(histogram.counts[0][50], 0u);
    EXPECT_EQ(histogram.counts[0][200], 0u);
}

TEST(MainWindowTest, HistogramIsNotComputedWhileHidden) {
    const auto window = makeTestWindow();
    showSized(*window);

    feedLive(*window, "a", liveFrame(50));
    QApplication::processEvents();
    feedLive(*window, "a", liveFrame(60));
    QApplication::processEvents();

    EXPECT_TRUE(stripOf(*window)->histogram().counts.empty());
}

TEST(MainWindowTest, DraggingOnTheHistogramSetsFixed) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    window->addImage("a", greyStill(128));
    QApplication::processEvents();
    ASSERT_EQ(viewedGrey(*window), 128);

    dragOnStrip(*stripOf(*window), 255, 200);

    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: Fixed ▾"));
    const viewer_core::DisplaySettings settings = displayControlOf(*window)->settings();
    EXPECT_EQ(settings.mode, viewer_core::RangeMode::Fixed);
    EXPECT_EQ(settings.range.min, 0.0);
    EXPECT_NEAR(settings.range.max, 200.0, 1.0);
    EXPECT_GT(viewedGrey(*window), 128);
}

TEST(MainWindowTest, DraggingUnderHdrKeepsHdr) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    feedStill(*window, "phone", cv::Mat(40, 40, CV_16UC3, cv::Scalar::all(20000)), kHlgDescription);
    const int before = viewedGrey(*window);

    dragOnStrip(*stripOf(*window), 65535, 40000);

    EXPECT_EQ(buttonText(*window, "rangeButton"), QStringLiteral("Range: HDR ▾"));
    const viewer_core::DisplaySettings settings = displayControlOf(*window)->settings();
    EXPECT_EQ(settings.mode, viewer_core::RangeMode::Hdr);
    EXPECT_EQ(settings.range.min, 0.0);
    EXPECT_NEAR(settings.range.max, 40000.0, 0.01 * 65536);
    EXPECT_GT(viewedGrey(*window), before);
}

TEST(MainWindowTest, DoubleClickReturnsToTheDefaultRange) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    window->addImage("grey", greyStill(128));
    entryTitled(displayControlOf(*window)->coloursMenu(), QStringLiteral("Turbo"))->trigger();
    dragOnStrip(*stripOf(*window), 255, 200);
    ASSERT_EQ(displayControlOf(*window)->settings().mode, viewer_core::RangeMode::Fixed);

    doubleClickStrip(*stripOf(*window), 100);

    EXPECT_EQ(displayControlOf(*window)->settings(),
              (viewer_core::DisplaySettings{.colours = viewer_core::ColourMap::Turbo}));

    feedStill(*window, "phone", cv::Mat(40, 40, CV_16UC3, cv::Scalar::all(20000)), kHlgDescription);
    dragOnStrip(*stripOf(*window), 65535, 40000);
    ASSERT_NE(displayControlOf(*window)->settings().range, viewer_core::DisplayRange{});

    doubleClickStrip(*stripOf(*window), 20000);

    EXPECT_EQ(displayControlOf(*window)->settings(),
              (viewer_core::DisplaySettings{.mode = viewer_core::RangeMode::Hdr}));
}

TEST(MainWindowTest, HistogramDragSurvivesAFrameKindChange) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    feedLive(*window, "camera", liveFrame(100));
    QApplication::processEvents();
    HistogramStrip& strip = *stripOf(*window);

    pressOnStrip(strip, 255);
    feedLive(*window, "camera", cv::Mat(30, 40, CV_16UC3, cv::Scalar::all(30000)),
             std::chrono::system_clock::now(), std::nullopt, kHlgDescription);
    QApplication::processEvents();
    moveOnStrip(strip, 200);
    QApplication::processEvents();
    releaseOnStrip(strip, 200);
    QApplication::processEvents();

    const viewer_core::DisplaySettings settings = displayControlOf(*window)->settings();
    EXPECT_EQ(settings.mode, viewer_core::RangeMode::Fixed); // the drag began on the 8-bit frame's markers
    EXPECT_LE(settings.range.max, 255.0);
    EXPECT_EQ(strip.histogram().counts.size(), 3u);
}

TEST(MainWindowTest, HistogramDragSurvivesAnAllNanFrame) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    cv::Mat depth(30, 40, CV_32FC1);
    cv::randu(depth, 0.0, 1.0);
    feedLive(*window, "depth", depth);
    QApplication::processEvents();
    HistogramStrip& strip = *stripOf(*window);
    ASSERT_TRUE(strip.markers());
    const viewer_core::DisplayRange range = strip.markers()->range;

    pressOnStrip(strip, range.max);
    feedLive(*window, "depth",
             cv::Mat(30, 40, CV_32FC1, cv::Scalar(std::numeric_limits<double>::quiet_NaN())));
    QApplication::processEvents();
    moveOnStrip(strip, (range.min + range.max) / 2);
    QApplication::processEvents();
    releaseOnStrip(strip, (range.min + range.max) / 2);
    QApplication::processEvents();

    const viewer_core::DisplaySettings settings = displayControlOf(*window)->settings();
    EXPECT_EQ(settings.mode, viewer_core::RangeMode::Fixed);
    EXPECT_TRUE(std::isfinite(settings.range.min));
    EXPECT_TRUE(std::isfinite(settings.range.max));
}

TEST(MainWindowTest, SwitchingImagesCancelsAHistogramDrag) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    window->addImage("a", greyStill(128));
    window->addImage("b", greyStill(128));
    HistogramStrip& strip = *stripOf(*window);

    pressOnStrip(strip, 255);
    moveOnStrip(strip, 200); // pending until the event loop turns
    ASSERT_TRUE(panelOf(*window)->selectImage(1));
    QApplication::processEvents();

    EXPECT_FALSE(strip.isDragging());
    EXPECT_EQ(displayControlOf(*window)->settings(), viewer_core::DisplaySettings{});
    ASSERT_TRUE(panelOf(*window)->selectImage(2));
    EXPECT_EQ(displayControlOf(*window)->settings(), viewer_core::DisplaySettings{});
}

// --- The live channel in view, converted on the converter's thread (quick win 7) ---------------------------

// The default conversion, held on the latch for the jobs `when` picks.
LiveConverter::Conversion holdingWhen(test_support::Latch& latch, std::function<bool(const LiveJob&)> when) {
    return [&latch, when = std::move(when)](const LiveJob& job) {
        if (when(job)) {
            latch.hold();
        }
        return convertLiveFrame(job);
    };
}

// For results that may not change the view (a dropped one, a redraw); where a test knows what the view will
// show, it waits for that instead, since a redraw of the frame before (after a resize, say) is a result too.
bool waitForLiveResults(MainWindow& window, std::uint64_t received) {
    return QTest::qWaitFor(
        [&window, received] { return window.liveResultCountsForTests().received >= received; });
}

bool waitForGrey(MainWindow& window, int grey) {
    return QTest::qWaitFor([&window, grey] { return viewedGrey(window) == grey; });
}

TEST(MainWindowTest, ThreadedLiveFramesReachTheView) {
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    showSized(window);

    feedLive(window, "camera", liveFrame(50));
    ASSERT_TRUE(waitForGrey(window, 50));
    feedLive(window, "camera", liveFrame(60));
    ASSERT_TRUE(waitForGrey(window, 60));
    window.tick(clocked.now);

    EXPECT_TRUE(infoDetails(window).contains(QStringLiteral("2 drawn"))) << infoDetails(window).toStdString();
}

// A new range, Show Clipping or a zoom draws the same frame again: not another frame drawn.
TEST(MainWindowTest, RedrawsOfOneFrameCountOnce) {
    ClockedWindow clocked;
    MainWindow& window = *clocked.window;
    showSized(window);
    showCamera(window, 50);

    applyFixedRange(window, QStringLiteral("0"), QStringLiteral("100"));
    action(window, "actionShowClipping")->trigger();
    window.tick(clocked.now);

    EXPECT_EQ(window.liveResultCountsForTests().applied, 3U);
    EXPECT_TRUE(infoDetails(window).contains(QStringLiteral("1 drawn"))) << infoDetails(window).toStdString();
}

// A frame whose job a newer frame replaced while the converter was busy never reached the screen: it counts
// with the dropped frames.
TEST(MainWindowTest, ReplacedJobsCountAsSkipped) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(
        holdingWhen(latch, [](const LiveJob& job) { return job.frameNumber == 1; }));
    showSized(window);

    feedLive(window, "camera", liveFrame(10));
    QApplication::processEvents(); // shown: frame 1's job is held
    ASSERT_TRUE(latch.waitUntilEntered());
    feedLive(window, "camera", liveFrame(20));
    QApplication::processEvents(); // frame 2's job waits
    feedLive(window, "camera", liveFrame(30));
    QApplication::processEvents(); // frame 3's replaces it
    latch.release();
    ASSERT_TRUE(waitForGrey(window, 30)); // frame 1's result came first
    window.tick(clocked.now);

    EXPECT_TRUE(infoDetails(window).contains(QStringLiteral("2 drawn · 1 dropped")))
        << infoDetails(window).toStdString();
}

// A frame redrawn (for a new range) while its first picture is still converting, its redraw then replaced by
// the next frame's job, did reach the screen: nothing was dropped.
TEST(MainWindowTest, ARedrawReplacedWhileItsFrameConvertsIsNotDropped) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(
        holdingWhen(latch, [](const LiveJob& job) { return job.keep == ImageView::KeepView::No; }));
    showSized(window);

    feedLive(window, "camera", liveFrame(10));
    QApplication::processEvents(); // shown: frame 1's first job, a fit, is held
    ASSERT_TRUE(latch.waitUntilEntered());
    applyFixedRange(window, QStringLiteral("0"), QStringLiteral("255")); // frame 1's redraw waits
    feedLive(window, "camera", liveFrame(30));
    QApplication::processEvents(); // frame 2's job replaces the redraw
    latch.release();
    ASSERT_TRUE(waitForGrey(window, 30)); // frame 1's picture came first
    window.tick(clocked.now);

    EXPECT_TRUE(infoDetails(window).contains(QStringLiteral("2 drawn · 0 dropped")))
        << infoDetails(window).toStdString();
}

TEST(MainWindowTest, ResultForAChannelNoLongerInViewIsDropped) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(
        holdingWhen(latch, [](const LiveJob& job) { return job.channel == "a"; }));
    showSized(window);

    feedLive(window, "a", liveFrame(50));
    QApplication::processEvents(); // a takes the view; its job is held
    ASSERT_TRUE(latch.waitUntilEntered());
    feedLive(window, "b", liveFrame(200));
    QApplication::processEvents();
    chooseLiveRow(window, QStringLiteral("b"));
    latch.release();
    ASSERT_TRUE(waitForGrey(window, 200)); // a's result came first

    const MainWindow::LiveResultCounts counts = window.liveResultCountsForTests();
    EXPECT_EQ(counts.received - counts.applied, 1U); // a's, dropped
    EXPECT_EQ(window.shownLiveChannel(), std::optional<std::string>("b"));
}

TEST(MainWindowTest, ResultAfterAStillTookTheViewIsDropped) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(holdingWhen(latch, [](const LiveJob&) { return true; }));
    showSized(window);

    feedLive(window, "camera", liveFrame(50));
    QApplication::processEvents(); // the channel takes the view; its job is held
    ASSERT_TRUE(latch.waitUntilEntered());
    window.addImage("still", greyStill(100));
    ASSERT_TRUE(panelOf(window)->selectImage(1));
    latch.release();
    ASSERT_TRUE(waitForLiveResults(window, 1));

    EXPECT_EQ(window.liveResultCountsForTests().applied, 0U);
    EXPECT_EQ(window.shownImageId(), std::optional<ImageId>(1));
    EXPECT_EQ(viewedGrey(window), 100);
}

// Shown again meanwhile, the channel's earlier job is for a view that has gone: only the new one is applied.
TEST(MainWindowTest, ResultFromAnEarlierViewOfTheSameChannelIsDropped) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(holdingWhen(latch, [](const LiveJob&) { return true; }));
    showSized(window);

    feedLive(window, "camera", liveFrame(50));
    QApplication::processEvents();
    ASSERT_TRUE(latch.waitUntilEntered());
    window.addImage("still", greyStill(100));
    ASSERT_TRUE(panelOf(window)->selectImage(1));
    chooseLiveRow(window, QStringLiteral("camera"));
    latch.release();
    ASSERT_TRUE(waitForGrey(window, 50)); // the earlier job's result came first

    const MainWindow::LiveResultCounts counts = window.liveResultCountsForTests();
    EXPECT_EQ(counts.received - counts.applied, 1U);
}

// A channel's first job fits the view to it, even when a newer frame's job replaced it before it ran.
TEST(MainWindowTest, FirstFrameOfAChannelIsFitted) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(
        holdingWhen(latch, [](const LiveJob& job) { return job.channel == "a" && job.frameNumber == 2; }));
    showSized(window);
    feedLive(window, "a", liveFrame(50));
    feedLive(window, "b", liveFrame(200)); // the same size
    QApplication::processEvents();
    ASSERT_TRUE(waitForGrey(window, 50));
    auto* view = window.findChild<ImageView*>();
    const QSize size(40, 30);
    zoomIn(*view, QRectF(view->rect()).center());
    ASSERT_NE(view->deviceScaleFor(size, ImageView::KeepView::IfSameSize),
              view->deviceScaleFor(size, ImageView::KeepView::No));

    feedLive(window, "a", liveFrame(60));
    QApplication::processEvents(); // a's frame 2: its job is held
    ASSERT_TRUE(latch.waitUntilEntered());
    chooseLiveRow(window, QStringLiteral("b")); // b's first job, to be fitted, waits
    feedLive(window, "b", liveFrame(210));
    QApplication::processEvents(); // b's next frame replaces it
    latch.release();
    ASSERT_TRUE(waitForGrey(window, 210));

    EXPECT_DOUBLE_EQ(view->deviceScaleFor(size, ImageView::KeepView::IfSameSize),
                     view->deviceScaleFor(size, ImageView::KeepView::No));
}

// A frame arriving while a channel's first picture converts is drawn for the fitted view that picture brings,
// not for the zoom of the picture still on screen: otherwise the fitted view would show only the part that
// zoom had in view, with the background around it, until something drew again.
TEST(MainWindowTest, AFrameArrivingWhileAFitConvertsIsDrawnForTheFit) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(holdingWhen(
        latch, [](const LiveJob& job) { return job.channel == "b" && job.keep == ImageView::KeepView::No; }));
    showSized(window);
    feedLive(window, "a", fourK());
    feedLive(window, "b", fourK()); // the same size
    QApplication::processEvents();
    ASSERT_TRUE(waitForLiveResults(window, 1));
    auto* view = window.findChild<ImageView*>();
    const std::uint64_t before = window.liveResultCountsForTests().received;
    zoomIn(*view, QRectF(view->rect()).center());
    ASSERT_TRUE(waitForLiveResults(window, before + 1));
    ASSERT_NE(view->pictureArea(), QRect(0, 0, 4000, 3000)); // a's zoomed-in cut

    chooseLiveRow(window, QStringLiteral("b")); // b's first job, to be fitted, is held converting
    ASSERT_TRUE(latch.waitUntilEntered());
    feedLive(window, "b", fourK());
    QApplication::processEvents(); // b's next frame's job waits behind it
    latch.release();
    ASSERT_TRUE(waitForLiveResults(window, before + 3));

    EXPECT_TRUE(view->pictureArea().contains(view->visibleArea()));
}

// A zoom whose job is still running, then a fit back: the picture ends at the fitted size, not the zoom's.
TEST(MainWindowTest, ZoomBackWhileAJobRunsEndsAtTheRightSize) {
    test_support::Latch latch;
    std::atomic<int> holdWiderThan = std::numeric_limits<int>::max(); // a zoom's picture, once known
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(holdingWhen(
        latch, [&holdWiderThan](const LiveJob& job) { return job.drawnSize.width > holdWiderThan; }));
    showSized(window);
    feedLive(window, "camera", fourK());
    QApplication::processEvents();
    ASSERT_TRUE(waitForLiveResults(window, 1));
    action(window, "actionFreeze")->trigger();
    auto* view = window.findChild<ImageView*>();
    const QSize fitted = view->picture().size();
    ASSERT_LT(fitted.width(), 4000);
    holdWiderThan = fitted.width();

    const QPointF centre = QRectF(view->rect()).center();
    const std::uint64_t before = window.liveResultCountsForTests().received;
    zoomIn(*view, centre); // a larger picture: its job is held
    ASSERT_TRUE(latch.waitUntilEntered());
    sendMouse(*view, QEvent::MouseButtonDblClick, centre, Qt::NoModifier); // fit again
    latch.release();
    ASSERT_TRUE(waitForLiveResults(window, before + 2));

    EXPECT_EQ(view->picture().size(), fitted);
}

TEST(MainWindowTest, ShowHistogramOnAFrozenChannelFillsTheStrip) {
    const auto window = makeTestWindow();
    showSized(*window);
    feedLive(*window, "camera", liveFrame(70));
    QApplication::processEvents();
    action(*window, "actionFreeze")->trigger();

    showHistogram(*window);

    EXPECT_TRUE(stripOf(*window)->isVisible());
    const viewer_core::Histogram& histogram = stripOf(*window)->histogram();
    ASSERT_EQ(histogram.counts.size(), 1U);
    EXPECT_GT(histogram.counts[0][70], 0U);
}

TEST(MainWindowTest, ThreadedHistogramComesWithTheFrame) {
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    showSized(window);
    showHistogram(window);

    feedLive(window, "camera", liveFrame(60));
    ASSERT_TRUE(waitForGrey(window, 60));

    EXPECT_TRUE(stripOf(window)->isVisible());
    const viewer_core::Histogram& histogram = stripOf(window)->histogram();
    ASSERT_EQ(histogram.counts.size(), 1U);
    EXPECT_GT(histogram.counts[0][60], 0U);
}

TEST(MainWindowTest, TurningTheHistogramOffHidesItWhileLive) {
    const auto window = makeTestWindow();
    showSized(*window);
    showHistogram(*window);
    feedLive(*window, "camera", liveFrame(60));
    QApplication::processEvents();
    ASSERT_TRUE(stripOf(*window)->isVisible());

    showHistogram(*window); // off

    EXPECT_FALSE(stripOf(*window)->isVisible());
    EXPECT_TRUE(stripOf(*window)->histogram().counts.empty());
}

// A frame converted with its histogram while the strip was on arrives after the strip was turned off.
TEST(MainWindowTest, ALateHistogramAfterTurningItOffStaysHidden) {
    test_support::Latch latch;
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    window.setLiveConversionForTests(
        holdingWhen(latch, [](const LiveJob& job) { return job.frameNumber == 2; }));
    showSized(window);
    showHistogram(window);
    feedLive(window, "camera", liveFrame(50));
    ASSERT_TRUE(waitForGrey(window, 50));
    ASSERT_TRUE(stripOf(window)->isVisible());

    feedLive(window, "camera", liveFrame(60));
    QApplication::processEvents(); // frame 2's job, histogram included, is held
    ASSERT_TRUE(latch.waitUntilEntered());
    showHistogram(window); // off
    latch.release();
    ASSERT_TRUE(waitForGrey(window, 60));

    EXPECT_FALSE(stripOf(window)->isVisible());
}

// What the histogram adds to each live frame the window takes and draws, on the UI thread: 4K 16UC3 HLG
// frames, the strip off and then on, median of 60 frames each. Run by hand in a release build:
// --gtest_also_run_disabled_tests --gtest_filter=*MainWindowMeasurement*
TEST(MainWindowMeasurement, DISABLED_HistogramFrameCost) {
    const auto window = makeTestWindow();
    showSized(*window);
    cv::Mat frame(2160, 3840, CV_16UC3);
    cv::randu(frame, cv::Scalar::all(0), cv::Scalar::all(65536));
    const auto medianPerFrame = [&window, &frame] {
        std::vector<double> times;
        for (int i = 0; i < 61; ++i) {
            QElapsedTimer timer;
            timer.start();
            feedLive(*window, "camera", frame, std::chrono::system_clock::now(), std::nullopt,
                     kHlgDescription);
            QApplication::processEvents();
            times.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
        }
        std::sort(times.begin() + 1, times.end()); // the first starts the channel
        return times[times.size() / 2];
    };

    const double off = medianPerFrame();
    showHistogram(*window);
    const double on = medianPerFrame();

    std::cout << "[ histogram ] 4K 16UC3 HLG frame taken and drawn: strip off " << off << " ms, on " << on
              << " ms, difference " << on - off << " ms\n";
}

// The UI thread's time in live frames (taking them and applying their results) is summed for the measurement
// below; a still costs it nothing.
TEST(MainWindowTest, LiveUiTimeCountsTakingAndApplying) {
    ClockedWindow clocked(LiveConversion::Threaded);
    MainWindow& window = *clocked.window;
    showSized(window);
    window.addImage("still", greyStill(10));
    EXPECT_EQ(window.liveUiTimeForTests().count(), 0);

    feedLive(window, "camera", liveFrame(60));
    QApplication::processEvents();
    const std::chrono::nanoseconds taken = window.liveUiTimeForTests();
    chooseLiveRow(window, QStringLiteral("camera"));
    ASSERT_TRUE(waitForGrey(window, 60));

    EXPECT_GT(taken.count(), 0);
    EXPECT_GT(window.liveUiTimeForTests(), taken);
}

// Waits for the window to apply one more live result, blocked on its event queue rather than polling, so the
// time it took is not rounded to a polling step. A timer wakes it to give up after five seconds.
bool waitForNextResult(MainWindow& window) {
    const std::uint64_t applied = window.liveResultCountsForTests().applied;
    QElapsedTimer waited;
    waited.start();
    QTimer wake;
    wake.start(100);
    while (window.liveResultCountsForTests().applied == applied) {
        if (waited.elapsed() > 5000) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    }
    return true;
}

// How long the UI thread works on each live frame, taking it and applying its picture, and how long from its
// arrival to its picture in the view: 4K 16UC3 HLG frames, each waited for until drawn, converted immediately
// (on the UI thread, as before the converter) and threaded, the strip off and on, with OpenCV's own thread
// count and with two; median of 60 frames each. Run by hand in a release build:
// --gtest_also_run_disabled_tests --gtest_filter=*LiveFrameCost*
TEST(MainWindowMeasurement, DISABLED_LiveFrameCost) {
    cv::Mat frame(2160, 3840, CV_16UC3);
    cv::randu(frame, cv::Scalar::all(0), cv::Scalar::all(65536));
    const auto median = [](std::vector<double> values) {
        std::sort(values.begin() + 1, values.end()); // the first starts the channel
        return values[values.size() / 2];
    };
    const int openCvThreads = cv::getNumThreads();
    for (const int threads : {openCvThreads, 2}) {
        cv::setNumThreads(threads);
        for (const LiveConversion conversion : {LiveConversion::Immediate, LiveConversion::Threaded}) {
            for (const bool strip : {false, true}) {
                const auto window = test_support::makeClockedTestWindow(&Clock::now, conversion);
                showSized(*window);
                if (strip) {
                    showHistogram(*window);
                }
                std::vector<double> uiTimes;
                std::vector<double> toScreen;
                for (int i = 0; i < 61; ++i) {
                    const std::chrono::nanoseconds uiBefore = window->liveUiTimeForTests();
                    QElapsedTimer timer;
                    timer.start();
                    feedLive(*window, "camera", frame, std::chrono::system_clock::now(), std::nullopt,
                             kHlgDescription);
                    ASSERT_TRUE(waitForNextResult(*window));
                    toScreen.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
                    uiTimes.push_back(static_cast<double>((window->liveUiTimeForTests() - uiBefore).count()) /
                                      1e6);
                }
                std::cout << "[ live frame ] 4K 16UC3 HLG, "
                          << (conversion == LiveConversion::Threaded ? "threaded" : "immediate") << ", strip "
                          << (strip ? "on" : "off") << ", OpenCV threads " << threads << ": UI thread "
                          << median(uiTimes) << " ms, arrival to view " << median(toScreen) << " ms\n";
            }
        }
    }
    cv::setNumThreads(openCvThreads);
}

// How long a live frame takes from arrival to its picture in the view when zoomed in, which is the
// conversion's time: 4K frames into a threaded window, 16UC3 HLG (shown as HDR), 16UC3 untagged and 32FC3
// untagged (Auto, which takes the whole frame's range), each waited for until drawn, zoomed once (four wheel
// notches, below 1:1) and past 1:1, the strip off and on; median of 60 frames each, and the area and picture
// converted. Run by hand in a release build: --gtest_also_run_disabled_tests
// --gtest_filter=*ZoomedLiveFrameCost*
TEST(MainWindowMeasurement, DISABLED_ZoomedLiveFrameCost) {
    cv::Mat sixteenBit(2160, 3840, CV_16UC3);
    cv::randu(sixteenBit, cv::Scalar::all(0), cv::Scalar::all(65536));
    cv::Mat floats(2160, 3840, CV_32FC3);
    cv::randu(floats, cv::Scalar::all(0.0), cv::Scalar::all(1.0));
    const QSize frameSize(3840, 2160);
    const auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    struct Kind {
        const char* name;
        cv::Mat frame;
        visual_debug::ImageDescription description;
    };
    for (const Kind& kind : {Kind{"16UC3 HLG", sixteenBit, kHlgDescription},
                             Kind{"16UC3 untagged", sixteenBit, {}}, Kind{"32FC3 untagged", floats, {}}}) {
        const cv::Mat& frame = kind.frame;
        const visual_debug::ImageDescription& description = kind.description;
        for (const bool pastOneToOne : {false, true}) {
            for (const bool strip : {false, true}) {
                const auto window =
                    test_support::makeClockedTestWindow(&Clock::now, LiveConversion::Threaded);
                showSized(*window);
                if (strip) {
                    showHistogram(*window);
                }
                feedLive(*window, "camera", frame, std::chrono::system_clock::now(), std::nullopt,
                         description);
                ASSERT_TRUE(waitForNextResult(*window));
                auto* view = window->findChild<ImageView*>();
                const QPointF centre = QRectF(view->rect()).center();
                zoomIn(*view, centre);
                for (int i = 0; pastOneToOne && i < 20 &&
                                view->deviceScaleFor(frameSize, ImageView::KeepView::IfSameSize) < 1.0;
                     ++i) {
                    zoomIn(*view, centre);
                }
                QTest::qWait(300); // the zoom's redraw
                std::vector<double> toView;
                for (int i = 0; i < 60; ++i) {
                    QElapsedTimer timer;
                    timer.start();
                    feedLive(*window, "camera", frame, std::chrono::system_clock::now(), std::nullopt,
                             description);
                    ASSERT_TRUE(waitForNextResult(*window));
                    toView.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
                }
                std::cout << "[ zoomed live frame ] 4K " << kind.name << ", threaded, "
                          << (pastOneToOne ? "zoomed past 1:1" : "zoomed once") << " ("
                          << view->deviceScaleFor(frameSize, ImageView::KeepView::IfSameSize)
                          << " device pixels per frame pixel), strip " << (strip ? "on" : "off") << ": area "
                          << view->pictureArea().width() << "x" << view->pictureArea().height()
                          << ", picture " << view->picture().width() << "x" << view->picture().height()
                          << ", arrival to view " << median(toView) << " ms\n";
            }
        }
    }
}

// A snapshot taken while 4K HLG frames stream at 30 fps into a threaded window with the histogram strip on,
// zoomed past 1:1 (a cut; the area in view converted for the view) and fitted (the whole frame): how long the
// Snapshot action holds the UI thread, how long until the snapshot is listed, and how many pictures the view
// applied in the second after. Run by hand: --gtest_also_run_disabled_tests
// --gtest_filter=*SnapshotWhileStreaming*
TEST(MainWindowMeasurement, DISABLED_SnapshotWhileStreaming) {
    const auto window = test_support::makeClockedTestWindow(&Clock::now, LiveConversion::Threaded);
    showSized(*window);
    showHistogram(*window);
    cv::Mat frame(2160, 3840, CV_16UC3);
    cv::randu(frame, cv::Scalar::all(0), cv::Scalar::all(65536));
    feedLive(*window, "camera", frame, std::chrono::system_clock::now(), std::nullopt, kHlgDescription);
    ASSERT_TRUE(waitForNextResult(*window));
    std::atomic<bool> stop = false;
    MainWindow* target = window.get();
    std::thread feeder([&stop, target, &frame] {
        while (!stop) {
            feedLive(*target, "camera", frame, std::chrono::system_clock::now(), std::nullopt,
                     kHlgDescription);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    auto* view = window->findChild<ImageView*>();
    for (const bool zoomed : {true, false}) {
        if (zoomed) {
            for (int i = 0;
                 i < 20 && view->deviceScaleFor(QSize(3840, 2160), ImageView::KeepView::IfSameSize) < 1.0;
                 ++i) {
                zoomIn(*view, QRectF(view->rect()).center());
                QTest::qWait(200);
            }
        } else {
            sendMouse(*view, QEvent::MouseButtonDblClick, QRectF(view->rect()).center(), Qt::NoModifier);
        }
        QTest::qWait(1000);
        for (int i = 0; i < 3; ++i) {
            const std::size_t listed = panelOf(*window)->listedIds().size();
            const auto applied = window->liveResultCountsForTests().applied;
            QElapsedTimer timer;
            timer.start();
            action(*window, "actionSnapshot")->trigger();
            const double held = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            ASSERT_TRUE(
                QTest::qWaitFor([&window, listed] { return panelOf(*window)->listedIds().size() > listed; }));
            const double toListed = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            QTest::qWait(static_cast<int>(std::max(0.0, 1000 - toListed)));
            std::cout << "[ snapshot ] " << (zoomed ? "zoomed in, " : "fitted, ")
                      << "4K HLG streaming: UI thread held " << held << " ms, listed after " << toListed
                      << " ms, " << window->liveResultCountsForTests().applied - applied
                      << " pictures applied in the second from it\n";
        }
    }
    stop = true;
    feeder.join();
}

} // namespace
