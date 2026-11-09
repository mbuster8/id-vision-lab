#include "main_window.h"

#include "about.h"
#include "activity_log_panel.h"
#include "appearance.h"
#include "application_identity.h"
#include "controls.h"
#include "display_settings_control.h"
#include "display_text.h"
#include "divider_splitter.h"
#include "eliding_label.h"
#include "epoch_time.h"
#include "histogram_strip.h"
#include "image_file.h"
#include "image_info_bar.h"
#include "image_sidebar.h"
#include "image_tags_conversion.h"
#include "image_view.h"
#include "live_channel_panel.h"
#include "live_drawing.h"
#include "outline_icons.h"
#include "qimage_conversion.h"
#include "shades.h"
#include "thumbnail_panel.h"
#include "ui_main_window.h"
#include "viewer_core/histogram.h"

#include "viewer_core/display_converter.h"
#include "viewer_core/pixel_inspector.h"
#include "viewer_core/region_statistics.h"
#include "viewer_core/save_name.h"
#include "visual_debug/wire_format.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iterator>
#include <string_view>
#include <utility>
#include <variant>

namespace {

constexpr int kImageViewSizePlaceholder = 1000;
constexpr int kDefaultLogHeight = 160;
constexpr std::chrono::milliseconds kLiveTick{500};
constexpr std::chrono::seconds kRejectionLinesEvery{1}; // at most one line per reason in this time
// The server's replies, which the sink counts as rejections like any other.
constexpr std::string_view kPausedReason = "viewer paused";
constexpr std::string_view kTooManyChannelsReason = "too many live channels";
// Refused live channels named in the log, once each; refusals of any more are only counted, so a sender that
// puts a frame number in the channel name costs the log one line a second, not one a frame.
constexpr std::size_t kNamedRefusedChannels = 8;
const QString kSidebarWidthKey = QStringLiteral("viewer/sidebarWidth");
const QString kLogOpenKey = QStringLiteral("viewer/logOpen");
const QString kHistogramShownKey = QStringLiteral("viewer/histogramShown");
const QString kLogHeightKey = QStringLiteral("viewer/logHeight");

QString picturesFolder() { return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation); }

QSize sizeOf(const cv::Mat& image) { return {image.cols, image.rows}; }

bool isTextField(const QWidget* widget) {
    return qobject_cast<const QLineEdit*>(widget) != nullptr ||
           qobject_cast<const QTextEdit*>(widget) != nullptr ||
           qobject_cast<const QPlainTextEdit*>(widget) != nullptr;
}

// Adds the time from construction to destruction to `total`, for MainWindowMeasurement; a nested one, such as
// an immediate result applied inside the take that made it, adds nothing of its own.
class LiveUiTiming {
public:
    LiveUiTiming(std::chrono::nanoseconds& total, int& depth) : m_total(total), m_depth(depth) {
        if (m_depth++ == 0) {
            m_timer.start();
        }
    }
    ~LiveUiTiming() {
        if (--m_depth == 0) {
            m_total += std::chrono::nanoseconds(m_timer.nsecsElapsed());
        }
    }
    LiveUiTiming(const LiveUiTiming&) = delete;
    LiveUiTiming& operator=(const LiveUiTiming&) = delete;

private:
    std::chrono::nanoseconds& m_total;
    int& m_depth;
    QElapsedTimer m_timer;
};

} // namespace

QString viewerWindowTitle(const QString& imageName, const QString& endpoint) {
    const QString tcpScheme = QStringLiteral("tcp://");
    const QString shownEndpoint = endpoint.startsWith(tcpScheme) ? endpoint.mid(tcpScheme.size()) : endpoint;
    return imageName.isEmpty()
               ? QCoreApplication::translate("MainWindow", "Visual Debug Viewer — %1").arg(shownEndpoint)
               : QCoreApplication::translate("MainWindow", "Visual Debug Viewer — %1 — %2")
                     .arg(imageName, shownEndpoint);
}

MainWindow::MainWindow(visual_debug::ServerOptions serverOptions, const QString& dataRoot, QWidget* parent)
    : MainWindow(std::move(serverOptions), dataRoot, &Clock::now, parent) {}

MainWindow::MainWindow(visual_debug::ServerOptions serverOptions, const QString& dataRoot,
                       std::function<Clock::time_point()> clock, QWidget* parent, LiveConversion conversion)
    : QMainWindow(parent), m_ui(std::make_unique<Ui::MainWindow>()), m_clock(std::move(clock)),
      m_sink(m_clock), m_server(m_sink, serverOptions),
      m_endpoint(QString::fromStdString(serverOptions.endpoint)), m_allowRemote(serverOptions.allowRemote),
      m_liveConversion(conversion) {
    m_ui->setupUi(this);
    setLiveConversionForTests(convertLiveFrame);
    setSnapshotConversionForTests(convertSnapshot);
    setWindowTitle(viewerWindowTitle(QString(), m_endpoint));

    DataFolder::Problem dataProblem = DataFolder::Problem::None;
    m_dataFolder = DataFolder::open(dataRoot, m_endpoint, &dataProblem);
    m_log = new ActivityLog(m_dataFolder ? m_dataFolder->logFile() : QString(), this);
    m_dataRoot = dataRoot;
    m_storage = std::make_unique<StorageController>(m_dataFolder ? m_dataFolder->imagesFolder() : QString());
    connect(m_storage.get(), &StorageController::restoreFinished, this, &MainWindow::onRestoreFinished);
    connect(m_storage.get(), &StorageController::failed, this,
            [this](const QString& what) { log(LogKind::Error, what); });
    // Leftovers of viewers that crashed while the setting was on. A running viewer's folder is its own to
    // clean, at its next write.
    if (!keepImagesSetting()) {
        DataFolder::removeStoredImagesEverywhere(dataRoot, true);
    }

    m_sidebar = new ImageSidebar;
    m_thumbnails = m_sidebar->thumbnails();
    m_infoBar = new ImageInfoBar;
    m_histogramStrip = new HistogramStrip;
    m_imageView = new ImageView;
    // The info strip sits on top of the image, in the splitter's second section, the histogram under it.
    auto* imageArea = new QWidget;
    imageArea->setObjectName(QStringLiteral("imageArea"));
    auto* imageLayout = new QVBoxLayout(imageArea);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->setSpacing(0);
    imageLayout->addWidget(m_infoBar);
    imageLayout->addWidget(m_histogramStrip);
    imageLayout->addWidget(m_imageView, 1);
    auto* splitter = new DividerSplitter(Qt::Horizontal);
    splitter->setObjectName(QStringLiteral("mainSplitter"));
    splitter->addWidget(m_sidebar);
    splitter->addWidget(imageArea);
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    // The image area's size is a placeholder: it takes whatever the sidebar leaves.
    splitter->setSizes({QSettings().value(kSidebarWidthKey, ImageSidebar::kDefaultWidth).toInt(),
                        kImageViewSizePlaceholder});
    connect(splitter, &QSplitter::splitterMoved, this,
            [splitter] { QSettings().setValue(kSidebarWidthKey, splitter->sizes().value(0)); });
    // The log opens below everything else, and only the log keeps its height when the window grows.
    m_logPanel = new ActivityLogPanel(m_log);
    m_logSplitter = new DividerSplitter(Qt::Vertical);
    m_logSplitter->setObjectName(QStringLiteral("logSplitter"));
    m_logSplitter->addWidget(splitter);
    m_logSplitter->addWidget(m_logPanel);
    m_logSplitter->setStretchFactor(0, 1);
    m_logSplitter->setCollapsible(0, false);
    m_logSplitter->setCollapsible(1, false);
    connect(m_logSplitter, &QSplitter::splitterMoved, this,
            [this] { QSettings().setValue(kLogHeightKey, m_logSplitter->sizes().value(1)); });
    // The divider marks the status bar's top edge.
    auto* central = new QWidget;
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_logSplitter, 1);
    auto* statusBarDivider = new qt_common::DividerLine;
    statusBarDivider->setObjectName(QStringLiteral("statusBarDivider"));
    centralLayout->addWidget(statusBarDivider);
    setCentralWidget(central);
    qt_common::usePanelGrey(m_sidebar);
    qt_common::usePanelGrey(m_logPanel);
    // The keyboard starts on the thumbnails, so the arrow keys step through the images and the search field
    // shows its accent-coloured focus ring only once it is used.
    m_thumbnails->setFocus(Qt::OtherFocusReason);

    m_pixelLabel = new QLabel;
    m_pixelLabel->setObjectName(QStringLiteral("pixelReadout"));
    m_pixelLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // The platform's usual margin, so the readout does not touch the window's left edge.
    m_pixelLabel->setContentsMargins(style()->pixelMetric(QStyle::PM_LayoutLeftMargin), 0, 0, 0);
    auto* latestLabel = new ElidingLabel(Qt::ElideRight);
    latestLabel->setObjectName(QStringLiteral("latestLogEntry"));
    latestLabel->setForegroundRole(QPalette::PlaceholderText);
    latestLabel->setContentsMargins(style()->pixelMetric(QStyle::PM_LayoutLeftMargin), 0, 0, 0);
    m_latestLabel = latestLabel;
    m_logToggle = new QToolButton;
    m_logToggle->setObjectName(QStringLiteral("logToggle"));
    m_logToggle->setText(tr("Log"));
    m_logToggle->setCheckable(true);
    qt_common::frameButton(m_logToggle);
    m_logToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_logToggle->setLayoutDirection(Qt::RightToLeft); // "Log ▴": the chevron after the word
    m_logToggle->setToolTip(tr("Show or hide the activity log"));
    m_serverControls = new ServerControls;
    statusBar()->setFont(qt_common::regularFont());
    qt_common::usePanelGrey(statusBar());
    statusBar()->addWidget(m_pixelLabel);
    statusBar()->addWidget(m_latestLabel, 1);
    statusBar()->addPermanentWidget(m_serverControls);
    statusBar()->addPermanentWidget(m_logToggle);
    connect(m_logToggle, &QToolButton::toggled, this, &MainWindow::setLogOpen);
    connect(m_log, &QAbstractItemModel::rowsInserted, this, &MainWindow::showLatestEntry);
    connect(m_log, &QAbstractItemModel::modelReset, this, &MainWindow::showLatestEntry);
    setLogOpen(QSettings().value(kLogOpenKey, false).toBool());
    showLatestEntry();

    // The same shortcuts as the thumbnail icons' tooltips name.
    m_ui->actionRename->setShortcut(thumbnailActionShortcut(ThumbnailAction::Rename));
    m_ui->actionDuplicate->setShortcut(thumbnailActionShortcut(ThumbnailAction::Duplicate));
    m_ui->actionCopyImage->setShortcut(thumbnailActionShortcut(ThumbnailAction::CopyImage));
    m_ui->actionDelete->setShortcut(thumbnailActionShortcut(ThumbnailAction::Delete));

    connect(m_ui->actionOpen, &QAction::triggered, this, &MainWindow::openFile);
    connect(m_ui->actionSaveAs, &QAction::triggered, this, [this] {
        if (m_liveChannel) {
            saveLiveFrame();
        } else if (m_currentId) {
            saveImage(*m_currentId);
        } else {
            log(LogKind::Warning, tr("No image to save"));
        }
    });
    connect(m_ui->actionQuit, &QAction::triggered, this, &QWidget::close);
    connect(m_ui->actionUndoDelete, &QAction::triggered, this, &MainWindow::undoDelete);
    connect(m_ui->actionFind, &QAction::triggered, m_sidebar, &ImageSidebar::focusSearch);
    m_sidebar->populateViewMenu(m_ui->menuView);
    m_ui->menuView->addSeparator();
    m_ui->menuView->addAction(m_ui->actionShowLog);
    connect(m_ui->actionShowLog, &QAction::triggered, this, &MainWindow::setLogOpen);
    qt_common::addAppearanceMenu(m_ui->menuView);
    // The Image menu holds the info strip's own range and colour actions, in menus of its own: a native macOS
    // menu has one parent.
    DisplaySettingsControl* displayControl = m_infoBar->displayControl();
    for (QMenu* shared : {displayControl->rangeMenu(), displayControl->coloursMenu()}) {
        m_ui->menuImage->addMenu(shared->title())->addActions(shared->actions());
    }
    // Show Clipping follows them, and ends the info strip's Range menu too: added there only now, so the
    // Image menu's Range lists it once.
    m_ui->menuImage->addAction(m_ui->actionShowClipping);
    displayControl->rangeMenu()->addSeparator();
    displayControl->rangeMenu()->addAction(m_ui->actionShowClipping);
    connect(m_ui->actionShowClipping, &QAction::toggled, this, &MainWindow::setShowClipping);
    // Show Overlays, beside it and as a button in the info strip: one switch for the viewer, on at every
    // start. O works like Space for Freeze, from anywhere but a text field.
    m_ui->actionShowOverlays->setShortcuts(
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), QKeySequence(Qt::Key_O)});
    m_ui->menuImage->addAction(m_ui->actionShowOverlays);
    m_infoBar->setOverlayAction(m_ui->actionShowOverlays);
    connect(m_ui->actionShowOverlays, &QAction::toggled, m_imageView, &ImageView::setOverlaysShown);
    // Show Histogram, off at the first start and remembered after.
    m_ui->menuImage->addAction(m_ui->actionShowHistogram);
    m_ui->actionShowHistogram->setChecked(QSettings().value(kHistogramShownKey, false).toBool());
    connect(m_ui->actionShowHistogram, &QAction::toggled, this, &MainWindow::setHistogramShown);
    connect(m_histogramStrip, &HistogramStrip::rangeDragged, this, &MainWindow::applyDraggedRange);
    connect(m_histogramStrip, &HistogramStrip::resetRequested, this, &MainWindow::resetRangeFromHistogram);
    // Freeze and Snapshot act on the live channel in view. Space freezes from anywhere but a text field,
    // which takes printable keys before shortcuts.
    m_ui->actionFreeze->setShortcuts(
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), QKeySequence(Qt::Key_Space)});
    m_ui->actionFreeze->setIcon(qt_common::makeOutlineIcon(qt_common::OutlineIcon::Pause));
    m_ui->actionSnapshot->setIcon(qt_common::makeOutlineIcon(qt_common::OutlineIcon::Camera));
    m_ui->menuImage->addSeparator();
    m_ui->menuImage->addAction(m_ui->actionFreeze);
    m_ui->menuImage->addAction(m_ui->actionSnapshot);
    m_infoBar->setLiveActions(m_ui->actionFreeze, m_ui->actionSnapshot);
    connect(m_ui->actionFreeze, &QAction::triggered, this, &MainWindow::setLiveFrozen);
    connect(m_ui->actionSnapshot, &QAction::triggered, this, &MainWindow::takeSnapshot);
    displayControl->setAutoRangeProvider([this] {
        if (const auto* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr) {
            // The frame on screen, frozen or not.
            return viewer_core::autoRange(channel->shown, channel->shownTags);
        }
        const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr;
        return entry != nullptr ? viewer_core::autoRange(entry->original, entry->origin.tags)
                                : viewer_core::DisplayRange{};
    });
    connect(displayControl, &DisplaySettingsControl::settingsChanged, this,
            &MainWindow::changeDisplaySettings);
    qt_common::addHelpMenu(menuBar(), QCoreApplication::translate("About", qt_common::kViewerDescription));
    // Another viewer may have changed the shared setting since.
    const auto showKeepImages = [this] { m_ui->actionKeepImages->setChecked(keepImagesSetting()); };
    showKeepImages();
    connect(m_ui->menuHistory, &QMenu::aboutToShow, this, showKeepImages);
    connect(m_ui->actionKeepImages, &QAction::triggered, this, &MainWindow::setKeepImages);
    connect(m_ui->actionClearHistory, &QAction::triggered, this, &MainWindow::clearHistory);
    // The Edit menu acts on the image in the view.
    const auto onShownImage = [this](void (MainWindow::*act)(viewer_core::ImageId)) {
        return [this, act] {
            if (m_currentId) {
                (this->*act)(*m_currentId);
            }
        };
    };
    connect(m_ui->actionRename, &QAction::triggered, this, onShownImage(&MainWindow::startRename));
    connect(m_ui->actionDuplicate, &QAction::triggered, this, onShownImage(&MainWindow::duplicateImage));
    connect(m_ui->actionCopyImage, &QAction::triggered, this,
            [this, copyShown = onShownImage(&MainWindow::copyImage)] {
                if (m_liveChannel) {
                    copyLiveFrame();
                } else {
                    copyShown();
                }
            });
    connect(m_ui->actionDelete, &QAction::triggered, this, onShownImage(&MainWindow::deleteImage));
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::updateEditActions);
    qApp->installEventFilter(this); // Space for a focused button, before the Freeze shortcut
    connect(m_ui->actionPauseServer, &QAction::triggered, this, &MainWindow::togglePause);
    connect(m_ui->actionStopServer, &QAction::triggered, this, &MainWindow::toggleRunning);
    connect(m_serverControls, &ServerControls::pauseClicked, this, &MainWindow::togglePause);
    connect(m_serverControls, &ServerControls::stopClicked, this, &MainWindow::toggleRunning);

    connect(&m_sink, &QtImageSink::imageReceived, this, &MainWindow::onImageReceived, Qt::QueuedConnection);
    connect(&m_sink, &QtImageSink::requestsRejected, this, &MainWindow::logRejections, Qt::QueuedConnection);
    m_rejectionTimer = new QTimer(this);
    m_rejectionTimer->setSingleShot(true);
    m_rejectionTimer->setInterval(kRejectionLinesEvery);
    connect(m_rejectionTimer, &QTimer::timeout, this, &MainWindow::writeRejections);
    connect(&m_sink, &QtImageSink::liveFramesWaiting, this, &MainWindow::takeLiveFrames,
            Qt::QueuedConnection);
    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(kLiveTick);
    connect(m_liveTimer, &QTimer::timeout, this, [this] { tick(m_clock()); });
    LiveChannelPanel* livePanel = m_sidebar->liveChannels();
    connect(livePanel, &LiveChannelPanel::channelSelected, this,
            [this](const QString& name) { showLiveChannel(name.toStdString()); });
    connect(livePanel, &LiveChannelPanel::closeRequested, this,
            [this](const QString& name) { closeLiveChannel(name.toStdString()); });
    connect(livePanel, &LiveChannelPanel::muteRequested, this,
            [this](const QString& name, bool muted) { setLiveMuted(name.toStdString(), muted); });
    connect(m_thumbnails, &ThumbnailPanel::imageSelected, this, &MainWindow::showImage);
    m_thumbnails->setThumbnailMaker(
        [this](viewer_core::ImageId id, QSize deviceSize) { return thumbnailPicture(id, deviceSize); });
    connect(m_thumbnails, &ThumbnailPanel::actionRequested, this,
            [this](viewer_core::ImageId id, ThumbnailAction action) {
                switch (action) {
                case ThumbnailAction::Rename:
                    startRename(id);
                    break;
                case ThumbnailAction::Duplicate:
                    duplicateImage(id);
                    break;
                case ThumbnailAction::Delete:
                    deleteImage(id);
                    break;
                case ThumbnailAction::CopyImage:
                    copyImage(id);
                    break;
                case ThumbnailAction::SaveAs:
                    saveImage(id);
                    break;
                }
            });
    connect(m_infoBar, &ImageInfoBar::renameRequested, this, &MainWindow::renameImage);
    connect(m_imageView, &ImageView::pixelHovered, this, &MainWindow::showPixel);
    connect(m_imageView, &ImageView::hoverLeft, m_pixelLabel, &QLabel::clear);
    connect(m_imageView, &ImageView::scaleChanged, this, &MainWindow::redrawLiveFrameForView);
    connect(m_imageView, &ImageView::panned, this, &MainWindow::redrawLiveFrameForView);
    connect(m_imageView, &ImageView::regionChanged, this, &MainWindow::showRegionStatistics);
    updateEditActions();

    switch (dataProblem) {
    case DataFolder::Problem::None:
        break;
    case DataFolder::Problem::NoFolderName: // a wildcard port; any other such endpoint will not start either
        log(LogKind::Warning, tr("Not keeping images or the log: the port changes on every start"));
        break;
    case DataFolder::Problem::Locked:
        log(LogKind::Warning,
            tr("Another viewer uses this endpoint's data folder: keeping the log and images in memory only"));
        break;
    case DataFolder::Problem::Unavailable:
        log(LogKind::Warning,
            tr("Cannot use the data folder in [%1]: keeping the log and images in memory only")
                .arg(QDir::toNativeSeparators(dataRoot)));
        break;
    }

    // Listen only once the event loop runs: images accepted earlier would wait for a UI that is not yet
    // taking them, and a sender's first burst would find the viewer busy.
    setServerState(ServerState::Starting, tr("Starting…"));
    QTimer::singleShot(0, this, &MainWindow::startUp);
}

// Listening comes first, so senders are never refused while up to 2 GiB of stored images load.
void MainWindow::startUp() {
    startServer();
    m_storage->restore([this](const StoredImage& image) { return adoptRestored(image); });
}

// Also Start and Try again. A started server is never paused.
void MainWindow::startServer() {
    m_server.setPaused(false);
    if (const auto error = m_server.start()) {
        setServerState(ServerState::NotListening,
                       tr("Not listening: %1").arg(QString::fromStdString(error->reason)));
        return;
    }
    setServerState(ServerState::Listening, listeningText());
}

// Pause keeps the port and answers every sender "viewer paused", so senders get Rejected.
void MainWindow::togglePause() {
    switch (m_serverControls->state()) {
    case ServerState::Listening:
        m_server.setPaused(true);
        setServerState(ServerState::Paused, tr("Paused: senders get [viewer paused]"));
        break;
    case ServerState::Paused:
        m_server.setPaused(false);
        logPausedRejections();
        setServerState(ServerState::Listening, listeningText());
        break;
    case ServerState::Starting:
    case ServerState::Stopped:
    case ServerState::NotListening:
        break;
    }
}

// Stop releases the port, so senders get ViewerUnavailable; another viewer could take it meanwhile.
void MainWindow::toggleRunning() {
    switch (m_serverControls->state()) {
    case ServerState::Listening:
    case ServerState::Paused: {
        const QString endpoint = QString::fromStdString(m_server.boundEndpoint());
        m_server.stop();
        logPausedRejections();
        setServerState(ServerState::Stopped, tr("Stopped: released [%1]").arg(endpoint));
        break;
    }
    case ServerState::Stopped:
    case ServerState::NotListening:
        startServer();
        break;
    case ServerState::Starting:
        break;
    }
}

// The Server menu's titles follow the state, as the buttons do.
void MainWindow::setServerState(ServerState state, const QString& text) {
    const bool running = state == ServerState::Listening || state == ServerState::Paused;
    m_serverControls->setState(state, text);
    m_ui->actionPauseServer->setText(state == ServerState::Paused ? tr("Resume") : tr("Pause"));
    m_ui->actionPauseServer->setEnabled(running);
    m_ui->actionStopServer->setText(running || state == ServerState::Starting ? tr("Stop") : tr("Start"));
    m_ui->actionStopServer->setEnabled(state != ServerState::Starting);
    if (state != ServerState::Starting) {
        log(state == ServerState::NotListening ? LogKind::Error : LogKind::Info, text);
    }
}

QString MainWindow::listeningText() const {
    const QString endpoint = QString::fromStdString(m_server.boundEndpoint());
    return m_allowRemote ? tr("Listening on [%1] (remote connections allowed)").arg(endpoint)
                         : tr("Listening on [%1]").arg(endpoint);
}

// The sink counts rejections while the UI is busy, so a flood becomes one entry per reason; and a sender that
// keeps streaming into a refusal gets at most one line a second. Reasons are fixed ASCII texts of the
// server's, never the sender's bytes.
void MainWindow::logRejections() {
    if (!m_rejectionTimer->isActive()) {
        writeRejections();
    }
}

void MainWindow::writeRejections() {
    const bool paused = m_serverControls->state() == ServerState::Paused;
    bool logged = false;
    for (const auto& [reason, count] : m_sink.takeRejections()) {
        if (paused && reason == kPausedReason) {
            m_pausedRejections += count; // said once, at Resume or Stop
            continue;
        }
        if (reason == kTooManyChannelsReason && !std::exchange(m_unnamedRefusals, false)) {
            continue; // each refused channel was logged once by name
        }
        const QString text = QString::fromStdString(reason);
        log(LogKind::Warning, count == 1 ? tr("Rejected a request: [%1]").arg(text)
                                         : tr("Rejected %1 requests: [%2]").arg(count).arg(text));
        logged = true;
    }
    if (logged) {
        m_rejectionTimer->start();
    }
}

// Called with the server already resumed or stopped, and the controls still saying Paused, so the counts
// taken now are the paused ones.
void MainWindow::logPausedRejections() {
    writeRejections();
    const int count = std::exchange(m_pausedRejections, 0);
    if (count == 0) {
        return;
    }
    const QString reason =
        QString::fromUtf8(kPausedReason.data(), static_cast<qsizetype>(kPausedReason.size()));
    log(LogKind::Warning, count == 1 ? tr("Rejected a request while paused: [%1]").arg(reason)
                                     : tr("Rejected %1 requests while paused: [%2]").arg(count).arg(reason));
}

void MainWindow::log(LogKind kind, const QString& text) {
    m_log->append({std::chrono::system_clock::now(), kind, text});
}

void MainWindow::showLatestEntry() {
    const LogEntry* latest = m_log->latest();
    if (latest == nullptr) {
        m_latestLabel->clear();
        return;
    }
    m_latestLabel->setText(clockTime(toEpochMicroseconds(latest->time)) + u' ' + latest->text);
}

// Closed, the status bar shows the newest entry instead.
void MainWindow::setLogOpen(bool open) {
    {
        const QSignalBlocker blocker(m_logToggle); // this is what its toggled signal calls
        m_logToggle->setChecked(open);
    }
    m_ui->actionShowLog->setChecked(open); // emits toggled, not the triggered it is connected by
    m_logToggle->setIcon(qt_common::makeOutlineIcon(open ? qt_common::OutlineIcon::ChevronDown
                                                         : qt_common::OutlineIcon::ChevronUp));
    m_logPanel->setVisible(open);
    m_latestLabel->setVisible(!open);
    if (open) {
        // Before the window is laid out the splitter has no height: the placeholder then takes what the
        // log leaves, as the stretch factors say.
        const int height = QSettings().value(kLogHeightKey, kDefaultLogHeight).toInt();
        const int above =
            m_logSplitter->height() > height ? m_logSplitter->height() - height : kImageViewSizePlaceholder;
        m_logSplitter->setSizes({above, height});
    }
    QSettings().setValue(kLogOpenKey, open);
}

// Child widgets outlive this window's members, and destroying them moves focus and ends a rename: stop them,
// and the focus signal, reaching into a destroyed history.
MainWindow::~MainWindow() {
    m_converter.reset(); // waits for a frame being converted; no result arrives after this
    m_snapshots.reset(); // the same for a snapshot; those still waiting are dropped
    qApp->removeEventFilter(this);
    disconnect(qApp, &QApplication::focusChanged, this, nullptr);
    m_sidebar->liveChannels()->disconnect(this);
    m_thumbnails->disconnect(this);
    m_thumbnails->setThumbnailMaker({}); // it reads the history
    m_infoBar->disconnect(this);
    m_imageView->disconnect(this);
    m_storage.reset();
}

// Space is the Freeze shortcut, which would otherwise take it from a button that has the keyboard focus (with
// macOS keyboard navigation on, for one). Accepting the override sends the key to the button instead.
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Esc clears a region, unless a text field has the focus: there it ends an edit or clears a search.
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        m_imageView->region()) {
        const auto* widget = qobject_cast<QWidget*>(watched);
        const QWidget* focus = QApplication::focusWidget();
        const bool typing = qobject_cast<const QLineEdit*>(focus) != nullptr ||
                            qobject_cast<const QAbstractSpinBox*>(focus) != nullptr ||
                            qobject_cast<const QTextEdit*>(focus) != nullptr ||
                            qobject_cast<const QPlainTextEdit*>(focus) != nullptr;
        if (widget != nullptr && widget->window() == this && !typing) {
            m_imageView->clearRegion();
            return true;
        }
    }
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const auto* button = qobject_cast<QAbstractButton*>(watched);
        if (button != nullptr && button->window() == this && key->key() == Qt::Key_Space &&
            key->modifiers() == Qt::NoModifier) {
            event->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onImageReceived(const visual_debug::ImageMessage& message) {
    addImage(message.name, message.image,
             {.source = viewer_core::ImageSource::Received,
              .arrivedAt = message.receivedAt,
              .tags = toImageTags(message.description)});
    m_sink.markHandled();
}

bool MainWindow::addImage(std::string name, cv::Mat image, viewer_core::ImageOrigin origin) {
    viewer_core::ImageHistory::AddResult added;
    try {
        added = m_history.add(std::move(name), std::move(image), origin);
    } catch (const std::exception& error) {
        log(LogKind::Error, tr("Cannot show image: %1").arg(error.what()));
        return false;
    }
    listAdded(added, origin, std::nullopt);
    return true;
}

// Its picture was made on the snapshot converter's thread.
void MainWindow::addSnapshot(SnapshotResult result) {
    SnapshotJob& job = result.job;
    const QString name = singleLine(QString::fromStdString(job.name));
    viewer_core::ImageHistory::AddResult added;
    try {
        if (result.display.empty()) {
            throw std::runtime_error("its picture could not be made");
        }
        added = m_history.add(std::move(job.name), std::move(job.original), std::move(result.display),
                              job.origin);
    } catch (const std::exception& error) {
        log(LogKind::Error,
            tr("Could not take snapshot [%1]: %2").arg(name, QString::fromUtf8(error.what())));
        return;
    }
    listAdded(added, job.origin, job.area);
}

void MainWindow::listAdded(const viewer_core::ImageHistory::AddResult& added,
                           const viewer_core::ImageOrigin& origin, std::optional<cv::Rect> cutFrom) {
    // Timed when it arrived: for a received image, when the server thread finished decoding it. A snapshot is
    // logged when it reaches the history; its entry keeps its frame's receive time.
    const auto& entry = *m_history.find(added.id);
    const QString name = singleLine(QString::fromStdString(entry.name));
    const QString details = imageDetails(entry);
    // Each in one pass of arg(), so a name holding "%3" is never filled in itself.
    QString text = tr("Received [%1] (%2)").arg(name, details);
    if (origin.source == viewer_core::ImageSource::OpenedFile) {
        text = tr("Opened [%1] (%2)").arg(name, details);
    } else if (origin.source == viewer_core::ImageSource::LiveSnapshot && cutFrom) {
        text = tr("Snapshot [%1] (%2), cut from the frame at %3, %4")
                   .arg(name, details, QString::number(cutFrom->x), QString::number(cutFrom->y));
    } else if (origin.source == viewer_core::ImageSource::LiveSnapshot) {
        text = tr("Snapshot [%1] (%2)").arg(name, details);
    }
    const bool timed = origin.arrivedAt != std::chrono::system_clock::time_point{} &&
                       origin.source != viewer_core::ImageSource::LiveSnapshot;
    m_log->append({timed ? origin.arrivedAt : std::chrono::system_clock::now(), LogKind::Info, text});
    // While a rename is open, arrivals do not take the selection, so a burst cannot move the edit; while a
    // live channel is shown, they do not take the view. A snapshot never does: it reaches the history a
    // moment after it was taken, when something else may be in view. One the search hides arrives quietly: it
    // is listed later, if ever.
    m_thumbnails->addImage(entry, false);
    if (!m_infoBar->isRenaming() && !m_liveChannel &&
        origin.source != viewer_core::ImageSource::LiveSnapshot && m_thumbnails->isListed(added.id)) {
        m_thumbnails->selectImage(added.id);
    }
    m_storage->store(entry);
    removeEvicted(added.evicted);
    m_storage->sync(m_history.entries());
    updateEditActions();
}

std::optional<viewer_core::ImageId> MainWindow::shownImageId() const { return m_currentId; }

std::optional<std::string> MainWindow::shownLiveChannel() const { return m_liveChannel; }

QtImageSink& MainWindow::sinkForTests() { return m_sink; }

void MainWindow::renameImage(viewer_core::ImageId id, QString name) {
    const auto* entry = m_history.find(id);
    if (entry == nullptr) {
        return;
    }
    // The wire format's rule, so a renamed image obeys the same limit as a received one: valid UTF-8, cut
    // to kMaxNameBytes at a character boundary.
    std::string cut = visual_debug::sanitiseName(name.trimmed().toStdString());
    if (cut.empty() || cut == entry->name) {
        return;
    }
    const QString oldName = singleLine(QString::fromStdString(entry->name));
    // The display settings go with the name; the old name is left with none, not with an explicit Auto.
    const std::optional<viewer_core::DisplaySettings> settings = m_displaySettings.find(entry->name);
    setDisplaySettings(entry->name, std::nullopt);
    setDisplaySettings(cut, settings);
    m_history.rename(id, std::move(cut));
    log(LogKind::Info,
        tr("Renamed [%1] to [%2]").arg(oldName, singleLine(QString::fromStdString(entry->name))));
    m_thumbnails->updateImage(*entry);
    if (m_currentId == id) {
        showImageText(*entry);
    }
    m_storage->sync(m_history.entries());
}

void MainWindow::duplicateImage(viewer_core::ImageId id) {
    const auto duplicated = m_history.duplicate(id);
    if (!duplicated) {
        return;
    }
    log(LogKind::Info,
        tr("Duplicated [%1]").arg(singleLine(QString::fromStdString(m_history.find(id)->name))));
    // Evicted rows go first, so the panel's rows match the history when the copy goes in at its index.
    removeEvicted(duplicated->evicted);
    m_thumbnails->insertImage(*m_history.find(duplicated->id), historyIndexOf(duplicated->id), false);
    selectAndShow(duplicated->id);
    m_storage->store(*m_history.find(duplicated->id));
    m_storage->sync(m_history.entries());
    updateEditActions();
}

void MainWindow::deleteImage(viewer_core::ImageId id) {
    const auto* entry = m_history.find(id);
    if (entry == nullptr) {
        return;
    }
    const QString name = singleLine(QString::fromStdString(entry->name));
    if (!m_history.remove(id)) {
        return;
    }
    log(LogKind::Info, tr("Deleted [%1]").arg(name));
    dropFromView(id);
    m_storage->remove(id);
    m_storage->sync(m_history.entries());
    updateEditActions();
}

void MainWindow::undoDelete() {
    const auto restored = m_history.restoreLastRemoved();
    if (!restored) {
        return;
    }
    log(LogKind::Info,
        tr("Restored [%1]").arg(singleLine(QString::fromStdString(m_history.find(restored->id)->name))));
    removeEvicted(restored->evicted);
    m_thumbnails->insertImage(*m_history.find(restored->id), historyIndexOf(restored->id), false);
    selectAndShow(restored->id);
    m_storage->store(*m_history.find(restored->id)); // its file went when it was deleted
    m_storage->sync(m_history.entries());
    updateEditActions();
}

// The displayed 8-bit picture, range and colours included: other programs cannot take the original's depths.
void MainWindow::copyImage(viewer_core::ImageId id) {
    if (const auto* entry = m_history.find(id)) {
        QGuiApplication::clipboard()->setImage(shownPicture(*entry));
        log(LogKind::Info,
            tr("Copied [%1] to the clipboard").arg(singleLine(QString::fromStdString(entry->name))));
    }
}

void MainWindow::saveImage(viewer_core::ImageId id) {
    if (const auto* entry = m_history.find(id)) {
        savePicture(shownPicture(*entry), QString::fromStdString(entry->name));
    }
}

// Saves what is on screen (the 8-bit picture, range and colours included), not the original: PNG and JPEG
// cannot hold every depth. QImage::save is used because cv::imwrite would swap red and blue in an RGBA image.
void MainWindow::savePicture(const QImage& picture, const QString& name) {
    QString suggestedName = name;
    suggestedName.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    suggestedName = QString::fromStdString(viewer_core::saveNameStem(suggestedName.toStdString()));
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Displayed Image"), picturesFolder() + '/' + suggestedName + QStringLiteral(".png"),
        tr("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".png");
    }
    const QString shownPath = QDir::toNativeSeparators(path);
    if (picture.save(path)) {
        log(LogKind::Info, tr("Saved [%1]").arg(shownPath));
    } else {
        log(LogKind::Error, tr("Could not save [%1]").arg(shownPath));
    }
}

// Also leaves a live channel: a history image takes its place.
void MainWindow::showImage(viewer_core::ImageId id) {
    const auto* entry = m_history.find(id);
    if (entry == nullptr) {
        return;
    }
    m_histogramStrip->cancelDrag(); // a range dragged for the image before must not reach this one's name
    viewChanged();
    if (m_liveChannel) {
        m_liveChannel.reset();
        m_sink.protectLiveChannel(std::nullopt);
        m_sidebar->liveChannels()->setSelected(std::nullopt);
    }
    m_currentId = id;
    m_lastHistoryId = id;
    showPicture(shownPicture(*entry, m_showClipping), ImageView::KeepView::No, sizeOf(entry->original),
                entry->origin.tags);
    updateHistogram();
    m_infoBar->displayControl()->setState(settingsFor(entry->name, entry->origin.tags),
                                          entry->original.channels() == 1, entry->origin.tags.isHdr());
    showImageText(*entry);
    showRegionStatistics(); // a region kept for an image of the same size is now this one's
    updateEditActions();
}

void MainWindow::showPicture(const QImage& picture, ImageView::KeepView keep, QSize imageSize,
                             const viewer_core::ImageTags& tags, QRect pictureArea) {
    m_imageView->setImage(picture, keep, imageSize, tags.overlays, pictureArea);
    m_infoBar->setOverlayCount(static_cast<int>(tags.overlayList().size()));
}

// Of the still in view, with the settings it is drawn with; only while the strip is shown. A live channel's
// histogram is counted on the converter's thread with its picture, and set when that arrives.
void MainWindow::updateHistogram() {
    if (m_liveChannel) {
        return;
    }
    const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr;
    if (!m_ui->actionShowHistogram->isChecked() || entry == nullptr || entry->original.empty()) {
        m_histogramStrip->clear();
        m_histogramStrip->hide();
        return;
    }
    const cv::Mat& original = entry->original;
    const viewer_core::ValueExtent extent = viewer_core::valueExtent(original);
    const std::optional<viewer_core::RangeMarkers> markers =
        viewer_core::rangeMarkers(original.depth(), original.channels(),
                                  settingsFor(entry->name, entry->origin.tags), entry->origin.tags, extent);
    m_histogramStrip->setHistogram(viewer_core::histogramOf(original, extent, markers), markers);
    m_histogramStrip->show();
}

// Turned on over a live channel, its frame is converted again with the histogram; turned off, the strip goes
// at once, and a histogram still on its way is not shown.
void MainWindow::setHistogramShown(bool shown) {
    QSettings().setValue(kHistogramShownKey, shown);
    if (!m_liveChannel) {
        updateHistogram();
    } else if (shown) {
        submitLiveFrame(ImageView::KeepView::IfSameSize);
    } else {
        m_histogramStrip->clear();
        m_histogramStrip->hide();
    }
}

// The strip says whether its drag began on HDR's markers; it keeps those markers for the whole drag, so a
// frame that arrives meanwhile, of another kind or with nothing finite, never changes what the drag means.
void MainWindow::applyDraggedRange(viewer_core::DisplayRange range, bool hdr) {
    if (!m_liveChannel && !m_currentId) {
        return;
    }
    DisplaySettingsControl* control = m_infoBar->displayControl();
    control->applySettings(viewer_core::settingsForDrag(control->settings(), hdr, range));
}

// As choosing the name's default in the Range menu: Auto, or HDR for HLG and PQ, with the colour map kept.
void MainWindow::resetRangeFromHistogram() {
    const viewer_core::ImageTags* tags = nullptr;
    if (const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr) {
        tags = &channel->shownTags;
    } else if (const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr) {
        tags = &entry->origin.tags;
    }
    if (tags == nullptr) {
        return;
    }
    DisplaySettingsControl* control = m_infoBar->displayControl();
    viewer_core::DisplaySettings settings = control->settings();
    settings.mode = viewer_core::defaultSettings(*tags).mode;
    settings.range = {};
    control->applySettings(settings);
}

QImage MainWindow::shownPicture(const viewer_core::ReceivedImage& entry, bool showClipping) const {
    const viewer_core::DisplaySettings settings = settingsFor(entry.name, entry.origin.tags);
    return qt_common::toQImage(
        settings == viewer_core::defaultSettings(entry.origin.tags) && !showClipping
            ? entry.display
            : viewer_core::toDisplayImage(entry.original, settings,
                                          {.tags = entry.origin.tags, .showClipping = showClipping}));
}

// Of the original in view, the live channel's shown frame included; nothing without a region, whose label the
// view has cleared itself.
void MainWindow::showRegionStatistics() {
    const std::optional<QRect> region = m_imageView->region();
    if (!region) {
        return;
    }
    const cv::Mat* original = nullptr;
    if (const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr) {
        original = &channel->shown;
    } else if (const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr) {
        original = &entry->original;
    }
    if (original == nullptr || original->empty()) {
        return;
    }
    const viewer_core::RegionStatistics statistics = viewer_core::regionStatistics(
        *original, cv::Rect(region->x(), region->y(), region->width(), region->height()));
    m_imageView->setRegionText(regionLines(statistics, original->channels(), original->depth()));
}

// For the session, and for the view only: what is copied, saved or listed is never marked.
void MainWindow::setShowClipping(bool show) {
    m_showClipping = show;
    if (m_liveChannel) {
        submitLiveFrame(ImageView::KeepView::IfSameSize);
    } else if (const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr) {
        showPicture(shownPicture(*entry, m_showClipping), ImageView::KeepView::IfSameSize,
                    sizeOf(entry->original), entry->origin.tags);
    }
}

viewer_core::DisplaySettings MainWindow::settingsFor(const std::string& name,
                                                     const viewer_core::ImageTags& tags) const {
    return m_displaySettings.find(name).value_or(viewer_core::defaultSettings(tags));
}

// For the shown image's name, so every image of that name, and later arrivals, look the same: in the view
// and in the thumbnails, which a still sharing a live channel's name has too.
void MainWindow::changeDisplaySettings(const viewer_core::DisplaySettings& settings) {
    if (m_liveChannel) {
        setDisplaySettings(*m_liveChannel, settings);
        submitLiveFrame(ImageView::KeepView::IfSameSize);
        return;
    }
    const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr;
    if (entry == nullptr) {
        return;
    }
    setDisplaySettings(entry->name, settings);
    showPicture(shownPicture(*entry, m_showClipping), ImageView::KeepView::IfSameSize,
                sizeOf(entry->original), entry->origin.tags);
    updateHistogram(); // its markers follow the range
}

// Also forgets what was drawn with the name's old settings: its thumbnails, made again when next painted, and
// its live row's picture, made again at the next tick.
void MainWindow::setDisplaySettings(const std::string& name,
                                    const std::optional<viewer_core::DisplaySettings>& settings) {
    if (settings) {
        m_displaySettings.set(name, *settings);
    } else {
        m_displaySettings.forget(name);
    }
    m_thumbnails->refreshThumbnails(QString::fromStdString(name));
    m_rowPictures.erase(name);
}

// A thumbnail with its name's display settings, made at its own size; nothing for its tags' default ones,
// whose picture the history already holds.
cv::Mat MainWindow::thumbnailPicture(viewer_core::ImageId id, QSize deviceSize) const {
    const auto* entry = m_history.find(id);
    if (entry == nullptr) {
        return {};
    }
    const viewer_core::DisplaySettings settings = settingsFor(entry->name, entry->origin.tags);
    if (settings == viewer_core::defaultSettings(entry->origin.tags)) {
        return {};
    }
    return viewer_core::toDisplayImage(
        entry->original, settings,
        {.size = cv::Size(std::max(deviceSize.width(), 1), std::max(deviceSize.height(), 1)),
         .tags = entry->origin.tags});
}

// --- Live channels ----------------------------------------------------------------------------------------

// Everything the sink kept since the last take: the newest frame of each channel, the channels it refused,
// and the idle ones it closed to make room. Only the shown channel's frame is converted and drawn; the rest
// waits for the tick.
void MainWindow::takeLiveFrames() {
    const LiveUiTiming timing(m_liveUiTime, m_liveUiDepth);
    QtImageSink::LiveBatch batch = m_sink.takeLiveFrames();
    const Clock::time_point now = m_clock();
    LiveChannelPanel* panel = m_sidebar->liveChannels();

    // An eviction made room for a channel that opened at the same moment, so its frame is in this batch.
    std::vector<std::string> opened;
    for (const QtImageSink::LiveFrame& frame : batch.frames) {
        if (m_live.find(frame.message.name) == nullptr) {
            opened.push_back(frame.message.name);
        }
    }
    for (std::size_t i = 0; i < batch.evicted.size(); ++i) {
        const QString idle = singleLine(QString::fromStdString(batch.evicted[i]));
        log(LogKind::Info, i < opened.size() ? tr("Live channel [%1] replaced idle channel [%2]")
                                                   .arg(singleLine(QString::fromStdString(opened[i])), idle)
                                             : tr("Closed idle live channel [%1] to make room").arg(idle));
        forgetLiveChannel(batch.evicted[i]);
    }

    for (QtImageSink::LiveFrame& frame : batch.frames) {
        const std::string name = frame.message.name;
        const viewer_core::LiveChannel* before = m_live.find(name);
        const std::pair<std::uint64_t, std::uint64_t> countsBefore =
            before != nullptr
                ? std::pair{before->received + before->dropped, before->dropped + before->skipped}
                : std::pair<std::uint64_t, std::uint64_t>{};
        const viewer_core::LiveChannels::ReceiveResult result =
            m_live.receive({.channel = name,
                            .image = std::move(frame.message.image),
                            .receivedAt = frame.message.receivedAt,
                            .sentAt = frame.message.sentAt,
                            .replaced = frame.replaced,
                            .tags = toImageTags(frame.message.description)},
                           now);
        const bool muted = m_mutedChannels.contains(name);
        if (result.started) {
            if (!muted) {
                log(LogKind::Info,
                    tr("Live channel [%1] started").arg(singleLine(QString::fromStdString(name))));
            }
            m_startCounts[name] = countsBefore;
            m_refusedChannels.erase(name);
            panel->addChannel(QString::fromStdString(name));
            updateLiveRow(*m_live.find(name), now);
            m_liveTimer->start();
        }
        if (muted) {
            continue; // counted, never shown
        }
        if (!m_currentId && !m_liveChannel) {
            showLiveChannel(name); // the first channel, when nothing else is shown
        } else if (m_liveChannel == name && result.shownChanged) {
            submitLiveFrame(ImageView::KeepView::IfSameSize);
        }
    }

    for (const std::string& name : batch.refused) {
        if (m_refusedChannels.contains(name)) {
            continue;
        }
        if (m_refusedChannels.size() >= kNamedRefusedChannels) {
            m_unnamedRefusals = true; // counted with the rejections instead
            continue;
        }
        m_refusedChannels.insert(name);
        const QString shown = singleLine(QString::fromStdString(name));
        const auto open = static_cast<int>(m_live.channels().size());
        // An open channel is refused only for a frame that would take the frames past the byte budget.
        log(LogKind::Warning,
            m_live.find(name) == nullptr && open >= QtImageSink::kMaxLiveChannels
                ? tr("Refused live channel [%1]: %2 channels are open").arg(shown).arg(open)
                : tr("Refused live channel [%1]: the open channels' frames would take more than %2")
                      .arg(shown, QLocale().formattedDataSize(static_cast<qint64>(QtImageSink::kMaxLiveBytes),
                                                              0, QLocale::DataSizeIecFormat)));
    }
}

// From a click on its row, or the first channel when nothing else is shown. The view is fitted to it; its
// frames to come keep the zoom.
void MainWindow::showLiveChannel(const std::string& name) {
    const viewer_core::LiveChannel* channel = m_live.find(name);
    if (channel == nullptr || m_liveChannel == name || m_mutedChannels.contains(name)) {
        return;
    }
    m_histogramStrip->cancelDrag();
    viewChanged();
    m_currentId.reset();
    m_liveChannel = name;
    m_sink.protectLiveChannel(name); // never closed to make room while in view
    m_thumbnails->clearChoice();
    m_sidebar->liveChannels()->setSelected(QString::fromStdString(name));
    m_infoBar->displayControl()->setState(settingsFor(name, channel->shownTags),
                                          channel->shown.channels() == 1, channel->shownTags.isHdr());
    m_ui->actionFreeze->setChecked(channel->frozen); // each channel is frozen or not by itself
    m_pixelLabel->clear();
    submitLiveFrame(ImageView::KeepView::No);
    showLiveText(m_clock());
    showRegionStatistics();
    setWindowTitle(viewerWindowTitle(QString::fromStdString(name), m_endpoint));
    updateEditActions();
}

// A fit still converting was for what was in view before; its result will be dropped.
void MainWindow::viewChanged() {
    ++m_viewCounter;
    m_fitPending = false;
}

// Converted at the size it will be drawn when that is smaller than the frame: a 4K frame in a window costs no
// more than the window's pixels. Zoomed in, only the part in view and a margin. The job shares the frame's
// pixels and copies the rest, so the converter's thread reads nothing of the window's.
void MainWindow::submitLiveFrame(ImageView::KeepView keep) {
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr || channel->shown.empty()) {
        return;
    }
    const cv::Mat& frame = channel->shown;
    // Frames may change their channel count, or their tags.
    m_infoBar->displayControl()->setFrameKind(frame.channels() == 1, channel->shownTags.isHdr());
    const std::uint64_t frameNumber = channel->shownNumber;
    // While a fit converts, the zoom on screen is about to go: a picture cut for it would show only that part
    // of the fitted view. Fitting again as well covers a fit whose conversion fails.
    if (m_fitPending) {
        keep = ImageView::KeepView::No;
    }
    m_fitPending = keep == ImageView::KeepView::No;
    const LiveDrawing drawing = liveDrawing(*m_imageView, frame.size(), keep);
    std::optional<LiveJob> replaced =
        m_converter->submit({.viewCounter = m_viewCounter,
                             .channel = channel->name,
                             .frameNumber = frameNumber,
                             .frame = frame,
                             .settings = settingsFor(channel->name, channel->shownTags),
                             .tags = channel->shownTags,
                             .area = drawing.area,
                             .drawnSize = drawing.size,
                             .showClipping = m_showClipping,
                             .histogram = m_ui->actionShowHistogram->isChecked(),
                             .keep = keep});
    // Another frame of this channel replaced a frame not drawn yet. A job for it may still be converting, so
    // it is settled when a newer frame's picture is applied. A redraw of the same frame replacing its earlier
    // job loses nothing.
    channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr; // an immediate result may have redrawn
    if (replaced && channel != nullptr && replaced->channel == channel->name &&
        replaced->frameNumber != frameNumber && replaced->frameNumber > channel->drawnNumber) {
        m_replacedFrames[channel->name].insert(replaced->frameNumber);
    }
}

void MainWindow::applyLiveResult(LiveResult result) {
    const LiveUiTiming timing(m_liveUiTime, m_liveUiDepth);
    ++m_liveResultCounts.received;
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr || channel->name != result.job.channel ||
        result.job.viewCounter != m_viewCounter) {
        return; // made for what was in view before
    }
    ++m_liveResultCounts.applied;
    const ImageView::KeepView keep = result.job.keep;
    const cv::Rect& area = result.job.area;
    showPicture(result.picture, keep, result.imageSize, result.job.tags,
                QRect(area.x, area.y, area.width, area.height));
    // Off since the job was made: a histogram on its way stays hidden.
    if (result.histogram && m_ui->actionShowHistogram->isChecked()) {
        m_histogramStrip->setHistogram(*result.histogram, result.markers);
        m_histogramStrip->show();
    }
    const std::uint64_t frameNumber = result.job.frameNumber;
    m_live.markDrawn(channel->name, frameNumber, m_clock());
    // Results come in the order they were submitted, and a channel's frame numbers never go down: a replaced
    // frame below this one can no longer be drawn, so it was skipped, unless it is this one.
    if (const auto replaced = m_replacedFrames.find(channel->name); replaced != m_replacedFrames.end()) {
        std::set<std::uint64_t>& numbers = replaced->second;
        for (auto number = numbers.begin(); number != numbers.end() && *number <= frameNumber;
             number = numbers.erase(number)) {
            if (*number < frameNumber) {
                m_live.markSkipped(channel->name);
            }
        }
    }
    // The view fits only now: if the fitted view draws another area or size than the job assumed (the window
    // was resized meanwhile), draw it again for that.
    if (keep == ImageView::KeepView::No) {
        m_fitPending = false;
        const LiveDrawing fitted =
            liveDrawing(*m_imageView, cv::Size(result.imageSize.width(), result.imageSize.height()),
                        ImageView::KeepView::IfSameSize);
        if (fitted != LiveDrawing{area, cv::Size(result.picture.width(), result.picture.height())}) {
            submitLiveFrame(ImageView::KeepView::IfSameSize);
        }
    }
}

// After a zoom, a fit, a resize or a pan: a frame converted for the old view, a frozen one above all, would
// stay blurred, coarse or cut short; needsRedraw says when it would.
void MainWindow::redrawLiveFrameForView() {
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr || channel->shown.empty()) {
        return;
    }
    if (needsRedraw(*m_imageView, channel->shown.size(), m_converter->newestDrawing())) {
        submitLiveFrame(ImageView::KeepView::IfSameSize);
    }
}

void MainWindow::showLiveText(Clock::time_point now) {
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr) {
        return;
    }
    m_infoBar->showLiveChannel(QString::fromStdString(channel->name),
                               liveDetails(QString::fromStdString(viewer_core::describeImage(channel->shown)),
                                           *channel, m_live.stats(channel->name, now)));
}

void MainWindow::tick(Clock::time_point now) {
    for (const std::string& name : m_live.updateIdle(now)) {
        if (m_mutedChannels.contains(name)) {
            continue;
        }
        const viewer_core::LiveChannel* channel = m_live.find(name);
        const auto [arrivedBefore, lostBefore] = m_startCounts[name];
        // Dropped: the frames that never reached the screen, replaced in the sink or before conversion.
        log(LogKind::Info, tr("Live channel [%1] idle after %2 frames (%3 dropped)")
                               .arg(singleLine(QString::fromStdString(name)))
                               .arg(channel->received + channel->dropped - arrivedBefore)
                               .arg(channel->dropped + channel->skipped - lostBefore));
    }
    for (const viewer_core::LiveChannel& channel : m_live.channels()) {
        updateLiveRow(channel, now);
    }
    showLiveText(now);
    if (m_liveChannel) {
        showRegionStatistics(); // twice a second, not per frame: a region of a 4K frame takes a while
    }
    if (m_live.channels().empty()) {
        m_liveTimer->stop();
    }
}

// The row's picture is the channel's newest frame, the one set aside while frozen included, converted at the
// row's size: a 4K frame costs a row no more than a small one. With the shown frame's tags: a stream does not
// change them between frames.
void MainWindow::updateLiveRow(const viewer_core::LiveChannel& channel, Clock::time_point now) {
    auto& [madeAtFrame, picture] = m_rowPictures[channel.name];
    const cv::Mat& newest = channel.newest.empty() ? channel.shown : channel.newest;
    const bool muted = m_mutedChannels.contains(channel.name);
    if (!muted && (picture.isNull() || madeAtFrame != channel.received) && !newest.empty()) {
        const int height =
            std::max(1, static_cast<int>(std::lround(LiveChannelPanel::kRowHeight * devicePixelRatioF())));
        picture = qt_common::toQImage(viewer_core::toDisplayImage(
            newest, settingsFor(channel.name, channel.shownTags),
            {.size = viewer_core::sizeWithin(newest.size(), cv::Size(newest.cols, height)),
             .tags = channel.shownTags}));
        madeAtFrame = channel.received;
    }
    m_sidebar->liveChannels()->updateChannel(
        QString::fromStdString(channel.name), picture, m_live.stats(channel.name, now).arrivedPerSecond,
        {.idle = channel.idle, .frozen = channel.frozen, .muted = muted});
}

// Only idle channels have a close button: a channel that is still sending would start again at once.
void MainWindow::closeLiveChannel(const std::string& name) {
    if (m_live.find(name) == nullptr) {
        return;
    }
    m_sink.closeLiveChannel(name);
    forgetLiveChannel(name);
    log(LogKind::Info, tr("Closed live channel [%1]").arg(singleLine(QString::fromStdString(name))));
}

void MainWindow::forgetLiveChannel(const std::string& name) {
    m_live.close(name);
    m_rowPictures.erase(name);
    m_startCounts.erase(name);
    m_replacedFrames.erase(name); // a channel started again numbers its frames afresh
    m_sidebar->liveChannels()->removeChannel(QString::fromStdString(name));
    if (m_liveChannel == name) {
        leaveLiveChannel();
    }
}

void MainWindow::leaveLiveChannel() {
    viewChanged();
    m_liveChannel.reset();
    m_sink.protectLiveChannel(std::nullopt);
    m_sidebar->liveChannels()->setSelected(std::nullopt);
    if (m_lastHistoryId && m_history.find(*m_lastHistoryId) != nullptr) {
        selectAndShow(*m_lastHistoryId);
    } else {
        clearView();
    }
}

// Mute belongs to the name for the session. Muting also unfreezes the channel, which cannot be looked at
// while muted, and gives the view back if it was in it.
void MainWindow::setLiveMuted(const std::string& name, bool muted) {
    if (muted == m_mutedChannels.contains(name)) {
        return;
    }
    const QString shown = singleLine(QString::fromStdString(name));
    if (muted) {
        m_mutedChannels.insert(name);
        m_live.setFrozen(name, false);
        if (m_liveChannel == name) {
            leaveLiveChannel();
        }
        log(LogKind::Info, tr("Muted live channel [%1]").arg(shown));
    } else {
        m_mutedChannels.erase(name);
        m_rowPictures.erase(name); // from before the mute: the newest frame takes its place
        log(LogKind::Info, tr("Unmuted live channel [%1]").arg(shown));
    }
    if (const viewer_core::LiveChannel* channel = m_live.find(name)) {
        updateLiveRow(*channel, m_clock());
    }
}

// Frames go on arriving and being counted while frozen; unfreezing shows the newest.
void MainWindow::setLiveFrozen(bool frozen) {
    if (!m_liveChannel) {
        return;
    }
    if (m_live.setFrozen(*m_liveChannel, frozen)) {
        submitLiveFrame(ImageView::KeepView::IfSameSize);
    }
    const Clock::time_point now = m_clock();
    updateLiveRow(*m_live.find(*m_liveChannel), now);
    showLiveText(now);
}

// What is in view of the frame, frozen or not, into the history under "<channel> <its receive time>", with
// the channel's display settings, so it looks as it did on screen: zoomed in, the visible pixels cut from the
// original at full resolution, the shapes over them moved with the cut; fitted, the whole frame. Its picture
// is made on the snapshot converter's thread, so the stream in view never waits for it. The channel stays in
// view.
void MainWindow::takeSnapshot() {
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr || channel->shown.empty()) {
        return;
    }
    const qint64 milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(channel->shownReceivedAt.time_since_epoch())
            .count();
    // A channel's name may already be at the wire format's limit: the time is kept, the name cut.
    QString channelName = QString::fromStdString(channel->name);
    QString name = snapshotName(channelName, milliseconds);
    while (static_cast<std::size_t>(name.toUtf8().size()) > visual_debug::kMaxNameBytes &&
           !channelName.isEmpty()) {
        channelName.chop(channelName.back().isLowSurrogate() ? 2 : 1);
        name = snapshotName(channelName, milliseconds);
    }
    const std::string snapshot = name.toStdString();
    setDisplaySettings(snapshot, m_displaySettings.find(channel->name));
    SnapshotJob job{.name = snapshot,
                    .original = channel->shown,
                    .origin = {.source = viewer_core::ImageSource::LiveSnapshot,
                               .fileBytes = std::nullopt,
                               .arrivedAt = channel->shownReceivedAt,
                               .tags = channel->shownTags}};
    const QRect visible = m_imageView->visibleArea();
    const cv::Rect frame(0, 0, channel->shown.cols, channel->shown.rows);
    const cv::Rect area = cv::Rect(visible.x(), visible.y(), visible.width(), visible.height()) & frame;
    if (!area.empty() && area != frame) {
        // Copied, so a small snapshot does not keep the whole frame alive.
        job.original = channel->shown(area).clone();
        job.origin.tags = viewer_core::tagsForCrop(channel->shownTags, area.tl());
        job.area = area;
    }
    m_snapshots->submit(std::move(job));
}

QImage MainWindow::livePicture() const {
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr || channel->shown.empty()) {
        return {};
    }
    return qt_common::toQImage(viewer_core::toDisplayImage(
        channel->shown, settingsFor(channel->name, channel->shownTags), {.tags = channel->shownTags}));
}

void MainWindow::copyLiveFrame() {
    const QImage picture = livePicture();
    if (picture.isNull()) {
        return;
    }
    QGuiApplication::clipboard()->setImage(picture);
    log(LogKind::Info,
        tr("Copied [%1] to the clipboard").arg(singleLine(QString::fromStdString(*m_liveChannel))));
}

// Named as its snapshot would be, so frames saved from one stream do not overwrite each other.
void MainWindow::saveLiveFrame() {
    const viewer_core::LiveChannel* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr;
    if (channel == nullptr || channel->shown.empty()) {
        return;
    }
    const QImage picture =
        livePicture(); // the frame shown now, even if a newer one arrives during the dialog
    savePicture(picture, snapshotName(QString::fromStdString(channel->name),
                                      std::chrono::duration_cast<std::chrono::milliseconds>(
                                          channel->shownReceivedAt.time_since_epoch())
                                          .count()));
}

// Selecting it in the sidebar shows it; one the search hides is shown without a selection.
void MainWindow::selectAndShow(viewer_core::ImageId id) {
    if (!m_thumbnails->selectImage(id)) {
        showImage(id);
    }
}

void MainWindow::showImageText(const viewer_core::ReceivedImage& entry) {
    const QString name = QString::fromStdString(entry.name);
    m_infoBar->showImage(entry.id, name, imageDetails(entry));
    setWindowTitle(viewerWindowTitle(name, m_endpoint));
}

void MainWindow::clearView() {
    viewChanged();
    m_currentId.reset();
    m_liveChannel.reset();
    m_sink.protectLiveChannel(std::nullopt);
    m_sidebar->liveChannels()->setSelected(std::nullopt);
    m_imageView->clear();
    m_infoBar->clear();
    m_histogramStrip->clear();
    m_histogramStrip->hide();
    m_pixelLabel->clear();
    setWindowTitle(viewerWindowTitle(QString(), m_endpoint));
    updateEditActions();
}

// The thumbnail pencil selects its image first; the rename itself happens in the info strip.
void MainWindow::startRename(viewer_core::ImageId id) {
    if (m_history.find(id) == nullptr) {
        return;
    }
    if (m_currentId != id) {
        m_thumbnails->selectImage(id);
    }
    if (m_currentId == id) {
        m_infoBar->startRename();
    }
}

// The shared end of Delete and of eviction, once the image has left the history. A shown image is replaced
// before its row goes, so the list never picks the next image by itself.
void MainWindow::dropFromView(viewer_core::ImageId id) {
    if (m_infoBar->renamingId() == id) {
        m_infoBar->cancelRename();
    }
    if (m_currentId == id) {
        if (const auto next = listedNeighbour(id)) {
            m_thumbnails->selectImage(*next); // which shows it
        } else {
            clearView();
        }
    }
    m_thumbnails->removeImage(id);
}

void MainWindow::removeEvicted(const std::vector<viewer_core::ImageId>& evicted) {
    if (evicted.empty()) {
        return;
    }
    for (const auto id : evicted) {
        dropFromView(id);
        m_storage->remove(id);
    }
    log(LogKind::Info, evicted.size() == 1 ? tr("Dropped the oldest image")
                                           : tr("Dropped %1 oldest images").arg(evicted.size()));
}

// The next image down the list as shown, otherwise the one above, skipping rows whose images have already
// left the history (an eviction can take several at once). With the default order, the next older image,
// otherwise the next newer one. For an image the search hides, the first listed image.
std::optional<viewer_core::ImageId> MainWindow::listedNeighbour(viewer_core::ImageId id) const {
    const std::vector<viewer_core::ImageId> listed = m_thumbnails->listedIds();
    const auto inHistory = [this](viewer_core::ImageId candidate) {
        return m_history.find(candidate) != nullptr;
    };
    const auto position = std::find(listed.begin(), listed.end(), id);
    if (position == listed.end()) {
        const auto first = std::find_if(listed.begin(), listed.end(), inHistory);
        return first != listed.end() ? std::optional(*first) : std::nullopt;
    }
    if (const auto below = std::find_if(std::next(position), listed.end(), inHistory);
        below != listed.end()) {
        return *below;
    }
    if (const auto above = std::find_if(std::make_reverse_iterator(position), listed.rend(), inHistory);
        above != listed.rend()) {
        return *above;
    }
    return std::nullopt;
}

int MainWindow::historyIndexOf(viewer_core::ImageId id) const {
    const auto& entries = m_history.entries();
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [id](const viewer_core::ReceivedImage& entry) { return entry.id == id; });
    return static_cast<int>(std::distance(entries.begin(), found));
}

// Edit-menu actions follow focus: while a text field has it, Copy Image, Undo Delete and Delete are off, so
// ⌘C, ⌘Z and ⌘⌫ reach the field and act on its text.
void MainWindow::updateEditActions() {
    // A text field and the log panel keep the copy, undo and delete keys for their own text.
    QWidget* focus = QApplication::focusWidget();
    const bool typing = isTextField(focus) || (focus != nullptr && m_logPanel->isAncestorOf(focus));
    const bool shown = m_currentId.has_value();
    const bool live = m_liveChannel.has_value();
    m_ui->actionUndoDelete->setEnabled(m_history.undoDepth() > 0 && !typing);
    m_ui->actionRename->setEnabled(shown);
    m_ui->actionDuplicate->setEnabled(shown);
    m_ui->actionCopyImage->setEnabled((shown || live) && !typing);
    m_ui->actionDelete->setEnabled(shown && !typing);
    m_ui->actionClearHistory->setEnabled(m_history.size() > 0);
    m_ui->menuImage->setEnabled(shown || live);
    // Disabled, they leave Space to the thumbnails and buttons while no channel is shown.
    m_ui->actionFreeze->setEnabled(live);
    m_ui->actionSnapshot->setEnabled(live);
    if (!live) {
        m_ui->actionFreeze->setChecked(false);
    }
}

// The original values: of the history image, or of the live frame on screen.
void MainWindow::showPixel(int x, int y) {
    const cv::Mat* original = nullptr;
    if (const auto* channel = m_liveChannel ? m_live.find(*m_liveChannel) : nullptr) {
        original = &channel->shown;
    } else if (const auto* entry = m_currentId ? m_history.find(*m_currentId) : nullptr) {
        original = &entry->original;
    }
    const auto text = original != nullptr ? viewer_core::inspectPixel(*original, x, y) : std::nullopt;
    m_pixelLabel->setText(text ? QString::fromStdString(*text) : QString());
}

void MainWindow::openFile() {
    const QString path = qt_common::chooseImageFile(this);
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo file(path);
    const QString fileName = file.fileName();
    auto result = qt_common::readImageFile(path);
    if (const auto* error = std::get_if<QString>(&result)) {
        log(LogKind::Error, tr("Cannot open [%1]: %2").arg(fileName, *error));
        return;
    }
    addImage(fileName.toStdString(), std::get<cv::Mat>(std::move(result)),
             {.source = viewer_core::ImageSource::OpenedFile,
              .fileBytes = static_cast<std::uint64_t>(file.size()),
              .arrivedAt = std::chrono::system_clock::now()});
}

// --- Keeping images between sessions ----------------------------------------------------------------------

// Also how the constructor makes the converter: replacing it waits for the old one's thread.
void MainWindow::setLiveConversionForTests(LiveConverter::Conversion conversion) {
    m_converter.reset();
    m_converter = std::make_unique<LiveConverter>(
        m_liveConversion == LiveConversion::Threaded ? LiveConverter::Mode::Threaded
                                                     : LiveConverter::Mode::Immediate,
        [this](LiveResult result) { applyLiveResult(std::move(result)); }, std::move(conversion));
}

// The same for snapshots: one converter for the window's lifetime, made in the live converter's mode.
void MainWindow::setSnapshotConversionForTests(SnapshotConverter::Conversion conversion) {
    m_snapshots.reset();
    m_snapshots = std::make_unique<SnapshotConverter>(
        m_liveConversion == LiveConversion::Threaded ? SnapshotConverter::Mode::Threaded
                                                     : SnapshotConverter::Mode::Immediate,
        [this](SnapshotResult result) { addSnapshot(std::move(result)); }, std::move(conversion));
}

MainWindow::LiveResultCounts MainWindow::liveResultCountsForTests() const { return m_liveResultCounts; }

std::chrono::nanoseconds MainWindow::liveUiTimeForTests() const { return m_liveUiTime; }

void MainWindow::setConfirmationForTests(std::function<bool(const QString&)> confirm) {
    m_confirm = std::move(confirm);
}

// Restored images go below everything, arrivals during loading included, and only while they fit: arrivals
// take priority for the limits.
std::optional<viewer_core::ImageId> MainWindow::adoptRestored(const StoredImage& image) {
    std::optional<viewer_core::ImageHistory::AddResult> added;
    try {
        added = m_history.addOldest(image.name, image.original, image.origin);
    } catch (const std::exception&) {
        // decode() accepted it, so toDisplayImage can show it; counted as dropped all the same
    }
    if (!added) {
        return std::nullopt;
    }
    m_thumbnails->insertImage(*m_history.find(added->id), historyIndexOf(added->id), false);
    return added->id;
}

void MainWindow::onRestoreFinished(int restored, int skipped, int dropped) {
    if (restored + skipped + dropped > 0) {
        QString text = restored == 1 ? tr("Restored 1 image") : tr("Restored %1 images").arg(restored);
        if (skipped > 0) {
            text += tr("; skipped %1 damaged").arg(skipped);
        }
        if (dropped > 0) {
            text += tr("; dropped %1 that did not fit").arg(dropped);
        }
        log(skipped + dropped > 0 ? LogKind::Warning : LogKind::Info, text);
    }
    if (!m_currentId && !m_liveChannel && !m_infoBar->isRenaming()) {
        if (const std::vector<viewer_core::ImageId> listed = m_thumbnails->listedIds(); !listed.empty()) {
            m_thumbnails->selectImage(listed.front());
        }
    }
    m_storage->sync(m_history.entries());
    updateEditActions();
    emit restoreFinished();
}

// One setting for every viewer. Turning it on writes this viewer's history; turning it off deletes every
// viewer's stored images, after asking.
void MainWindow::setKeepImages(bool keep) {
    if (keep) {
        setKeepImagesSetting(true);
        for (const viewer_core::ReceivedImage& entry : m_history.entries()) {
            m_storage->store(entry);
        }
        m_storage->sync(m_history.entries());
        log(LogKind::Info, tr("Keeping images between sessions"));
        return;
    }
    const int stored = DataFolder::storedImageCountAll(m_dataRoot);
    const QString question =
        stored == 1 ? tr("Delete the stored image?") : tr("Delete the %1 stored images?").arg(stored);
    if (stored > 0 && !confirm(question)) {
        m_ui->actionKeepImages->setChecked(true);
        return;
    }
    // Off before deleting, so that a store already queued finds it off instead of writing after the deletion.
    // Each worker forgets its own images when it next acts.
    setKeepImagesSetting(false);
    DataFolder::removeStoredImagesEverywhere(m_dataRoot, false);
    QString text = tr("Not keeping images between sessions");
    if (stored > 0) {
        text += stored == 1 ? tr(": deleted 1 stored image") : tr(": deleted %1 stored images").arg(stored);
    }
    log(LogKind::Info, text);
}

void MainWindow::clearHistory() {
    const std::size_t count = m_history.size();
    const QString question =
        count == 1 ? tr("Delete the image in the history? This cannot be undone.")
                   : tr("Delete the %1 images in the history? This cannot be undone.").arg(count);
    if (count == 0 || !confirm(question)) {
        return;
    }
    if (m_infoBar->isRenaming()) {
        m_infoBar->cancelRename();
    }
    if (!m_liveChannel) {
        clearView(); // a live channel stays in view
    }
    m_lastHistoryId.reset();
    for (const viewer_core::ReceivedImage& entry : m_history.entries()) {
        m_thumbnails->removeImage(entry.id);
    }
    m_history.clear();
    m_storage->removeAll();
    log(LogKind::Info,
        count == 1 ? tr("Cleared the history: 1 image") : tr("Cleared the history: %1 images").arg(count));
    updateEditActions();
}

bool MainWindow::confirm(const QString& question) {
    if (m_confirm) {
        return m_confirm(question);
    }
    QMessageBox box(QMessageBox::Warning, windowTitle(), question, QMessageBox::Cancel, this);
    QPushButton* remove = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == remove;
}
