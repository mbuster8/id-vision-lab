#include "main_window.h"

#include "about.h"
#include "appearance.h"
#include "application_identity.h"
#include "camera_formats.h"
#include "image_tags_conversion.h"
#include "live_drawing.h"
#include "private_files.h"

#include <frame_io/capture.h>
#include <frame_io/image_folder.h>
#include <viewer_core/image_tags.h>
#include <visual_debug/client.h>
#include <visual_debug/endpoint.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace document_lab {

namespace {

const QString kFrameEntry = QStringLiteral("Frame");

constexpr std::array kProcessors{lab_core::ProcessorKind::None, lab_core::ProcessorKind::Edges,
                                 lab_core::ProcessorKind::Outline};

// A processor busy this long on one frame is named in the status bar.
constexpr auto kBusyShownAfter = std::chrono::seconds(1);
constexpr int kStatusRefreshMs = 500; // also how often the busy time moves on
constexpr int kClosePollMs = 100;     // while closing waits for the recording

QString qtString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// Paths through UTF-16, so a name in any script survives on every platform.
std::filesystem::path toPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }

QString fromPath(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }

// The machine an endpoint names: "192.168.1.20" of "tcp://192.168.1.20:7139", "[fe80::1]" of an IPv6 one.
QString hostOf(const QString& endpoint) {
    const QString rest = endpoint.section(QStringLiteral("://"), -1);
    if (rest.startsWith(u'[')) {
        return rest.section(u']', 0, 0) + u']';
    }
    const qsizetype colon = rest.lastIndexOf(u':');
    return colon > 0 ? rest.left(colon) : rest;
}

QString timeStamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")); }

QString stateText(lab_core::EngineState state) {
    switch (state) {
    case lab_core::EngineState::Idle:
        return LabWindow::tr("Ready");
    case lab_core::EngineState::Live:
        return LabWindow::tr("Live");
    case lab_core::EngineState::Playing:
        return LabWindow::tr("Playing");
    case lab_core::EngineState::Paused:
        return LabWindow::tr("Paused");
    case lab_core::EngineState::Ended:
        return LabWindow::tr("Ended");
    case lab_core::EngineState::Stopped:
        return LabWindow::tr("Stopped");
    }
    return {};
}

} // namespace

LabWindow::LabWindow(LabDependencies dependencies, QWidget* parent)
    : QMainWindow(parent), m_dependencies(std::move(dependencies)) {
    if (!m_dependencies.clock) {
        m_dependencies.clock = [] { return Clock::now(); };
    }
    if (!m_dependencies.makeProcessor) {
        m_dependencies.makeProcessor = [](lab_core::ProcessorKind kind) {
            return lab_core::makeProcessor(kind);
        };
    }
    if (!m_dependencies.conversion) {
        m_dependencies.conversion = convertLiveFrame;
    }
    if (!m_dependencies.makeSender) {
        m_dependencies.makeSender =
            [](const std::string& endpoint) -> std::unique_ptr<visual_debug::ImageSender> {
            return std::make_unique<visual_debug::Client>(visual_debug::ClientOptions{.endpoint = endpoint});
        };
    }
    buildUi();

    m_converter = std::make_unique<LiveConverter>(
        m_dependencies.conversionMode, [this](LiveResult result) { applyPicture(std::move(result)); },
        m_dependencies.conversion);
    // The engine's hooks come on its thread: they only post to this window's.
    lab_core::EngineHooks hooks;
    hooks.resultReady = [this] {
        QMetaObject::invokeMethod(this, [this] { takeResult(); }, Qt::QueuedConnection);
    };
    hooks.statusChanged = [this] {
        QMetaObject::invokeMethod(this, [this] { refreshStatus(); }, Qt::QueuedConnection);
    };
    hooks.now = m_dependencies.clock;
    m_engine = std::make_unique<lab_core::Engine>(m_dependencies.engineMode, std::move(hooks));

    chooseProcessor(m_processorCombo->currentIndex());
    if (m_send->isChecked()) {
        startSending(); // as last time; another computer's endpoint is asked about again
    }

    // The window is the context: frames reach it on its own thread, and none after it is gone.
    CameraBackend* cameras = m_dependencies.cameras.get();
    connect(cameras, &CameraBackend::camerasChanged, this, &LabWindow::fillCameras);
    connect(cameras, &CameraBackend::frameArrived, this, &LabWindow::cameraFrame);
    connect(cameras, &CameraBackend::failed, this, &LabWindow::cameraFailed);
    fillCameras();

    qApp->installEventFilter(this); // Space for the focused button, not Play
    refreshStatus();
    m_statusTimer->start(kStatusRefreshMs);
}

LabWindow::~LabWindow() {
    qApp->removeEventFilter(this);
    // The camera first: no frame reaches a window whose engine is gone.
    m_dependencies.cameras->disconnect(this);
    m_dependencies.cameras->stop();
    m_statusTimer->stop();
    m_engine.reset();    // joins: no hook comes after
    m_converter.reset(); // waits for a conversion in progress
}

void LabWindow::buildUi() {
    setWindowTitle(tr("Document Lab"));
    // View holds the appearance only, which Qt before 6.8 cannot offer; Help, the About box.
    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    if (qt_common::addAppearanceMenu(viewMenu) == nullptr) {
        viewMenu->menuAction()->setVisible(false);
    }
    qt_common::addHelpMenu(menuBar(),
                           QCoreApplication::translate("About", qt_common::kDocumentLabDescription));
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(new QLabel(tr("Camera:"), central));
    m_cameraCombo = new QComboBox(central);
    m_cameraCombo->setObjectName(QStringLiteral("cameraCombo"));
    m_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    sourceRow->addWidget(m_cameraCombo);
    m_formatCombo = new QComboBox(central);
    m_formatCombo->setObjectName(QStringLiteral("formatCombo"));
    m_formatCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    sourceRow->addWidget(m_formatCombo);
    m_startStop = new QPushButton(tr("Start"), central);
    m_startStop->setObjectName(QStringLiteral("startStopButton"));
    m_startStop->setEnabled(false);
    sourceRow->addWidget(m_startStop);
    m_openFile = new QPushButton(tr("Open File…"), central);
    m_openFile->setObjectName(QStringLiteral("openFileButton"));
    sourceRow->addWidget(m_openFile);
    m_openFolder = new QPushButton(tr("Open Folder…"), central);
    m_openFolder->setObjectName(QStringLiteral("openFolderButton"));
    sourceRow->addWidget(m_openFolder);
    sourceRow->addStretch();
    layout->addLayout(sourceRow);

    auto* replayRow = new QHBoxLayout;
    m_playPause = new QPushButton(tr("Play"), central);
    m_playPause->setObjectName(QStringLiteral("playPauseButton"));
    m_step = new QPushButton(tr("Step"), central);
    m_step->setObjectName(QStringLiteral("stepButton"));
    m_stepBack = new QPushButton(tr("Step Back"), central);
    m_stepBack->setObjectName(QStringLiteral("stepBackButton"));
    m_restart = new QPushButton(tr("Restart"), central);
    m_restart->setObjectName(QStringLiteral("restartButton"));
    m_loop = new QCheckBox(tr("Loop"), central);
    m_loop->setObjectName(QStringLiteral("loopCheck"));
    m_asFast = new QCheckBox(tr("As fast as possible"), central);
    m_asFast->setObjectName(QStringLiteral("asFastCheck"));
    for (QWidget* control :
         std::initializer_list<QWidget*>{m_playPause, m_step, m_stepBack, m_restart, m_loop, m_asFast}) {
        replayRow->addWidget(control);
    }
    replayRow->addStretch();
    layout->addLayout(replayRow);
    // Keys for the replay: Space, → and ←, from anywhere in the window but a focused button (eventFilter).
    m_playPauseAction = new QAction(tr("Play or Pause"), this);
    m_playPauseAction->setShortcut(QKeySequence(Qt::Key_Space));
    m_stepAction = new QAction(tr("Step"), this);
    m_stepAction->setShortcut(QKeySequence(Qt::Key_Right));
    m_stepBackAction = new QAction(tr("Step Back"), this);
    m_stepBackAction->setShortcut(QKeySequence(Qt::Key_Left));
    addActions({m_playPauseAction, m_stepAction, m_stepBackAction});

    auto* processingRow = new QHBoxLayout;
    processingRow->addWidget(new QLabel(tr("Processor:"), central));
    m_processorCombo = new QComboBox(central);
    m_processorCombo->setObjectName(QStringLiteral("processorCombo"));
    for (const lab_core::ProcessorKind kind : kProcessors) {
        m_processorCombo->addItem(qtString(lab_core::nameOf(kind)));
    }
    m_processorCombo->setCurrentIndex(std::max(0, m_processorCombo->findText(m_settings.processor())));
    processingRow->addWidget(m_processorCombo);
    processingRow->addWidget(new QLabel(tr("Show:"), central));
    m_showCombo = new QComboBox(central);
    m_showCombo->setObjectName(QStringLiteral("showCombo"));
    m_showCombo->addItem(kFrameEntry);
    m_showCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_wantedShow = m_settings.show();
    processingRow->addWidget(m_showCombo);
    m_overlaysCheck = new QCheckBox(tr("Overlays"), central);
    m_overlaysCheck->setObjectName(QStringLiteral("overlaysCheck"));
    m_overlaysCheck->setChecked(m_settings.overlays());
    processingRow->addWidget(m_overlaysCheck);
    processingRow->addStretch();
    m_record = new QPushButton(tr("Record"), central);
    m_record->setObjectName(QStringLiteral("recordButton"));
    m_record->setCheckable(true);
    processingRow->addWidget(m_record);
    m_capture = new QPushButton(tr("Capture"), central);
    m_capture->setObjectName(QStringLiteral("captureButton"));
    processingRow->addWidget(m_capture);
    m_openLabFolder = new QPushButton(tr("Open Lab Folder"), central);
    m_openLabFolder->setObjectName(QStringLiteral("openLabFolderButton"));
    processingRow->addWidget(m_openLabFolder);
    m_chooseLabFolder = new QPushButton(tr("Choose Folder…"), central);
    m_chooseLabFolder->setObjectName(QStringLiteral("chooseLabFolderButton"));
    processingRow->addWidget(m_chooseLabFolder);
    layout->addLayout(processingRow);

    auto* sendRow = new QHBoxLayout;
    m_send = new QCheckBox(tr("Send to viewer"), central);
    m_send->setObjectName(QStringLiteral("sendCheck"));
    m_send->setChecked(m_settings.sendToViewer());
    sendRow->addWidget(m_send);
    m_endpoint = new QLineEdit(m_settings.endpoint(), central);
    m_endpoint->setObjectName(QStringLiteral("endpointEdit"));
    m_endpoint->setPlaceholderText(QString::fromStdString(visual_debug::defaultEndpoint()));
    sendRow->addWidget(m_endpoint, 1);
    layout->addLayout(sendRow);

    // The canvas, with the placeholder over it in the same cell, beside the results.
    auto* splitter = new QSplitter(Qt::Horizontal, central);
    auto* canvasArea = new QWidget(splitter);
    auto* canvasLayout = new QGridLayout(canvasArea);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    m_canvas = new ImageView(canvasArea);
    m_canvas->setObjectName(QStringLiteral("canvas"));
    m_canvas->setRegionsEnabled(false);
    m_canvas->setOverlaysShown(m_overlaysCheck->isChecked());
    m_canvas->setEmptyText(tr("Start a camera, or open a video, an image or a folder of images."));
    canvasLayout->addWidget(m_canvas, 0, 0);
    m_placeholder = new QLabel(canvasArea);
    m_placeholder->setObjectName(QStringLiteral("placeholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setAutoFillBackground(true); // covers the picture of an earlier frame
    m_placeholder->hide();
    canvasLayout->addWidget(m_placeholder, 0, 0);
    m_results = new QListWidget(splitter);
    m_results->setObjectName(QStringLiteral("resultsList"));
    splitter->addWidget(canvasArea);
    splitter->addWidget(m_results);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    setCentralWidget(central);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("statusLabel"));
    statusBar()->addWidget(m_status, 1);
    m_statusTimer = new QTimer(this);
    m_closeTimer = new QTimer(this);

    connect(m_startStop, &QPushButton::clicked, this, &LabWindow::startOrStop);
    connect(m_openFile, &QPushButton::clicked, this, &LabWindow::openFile);
    connect(m_openFolder, &QPushButton::clicked, this, &LabWindow::openFolder);
    connect(m_playPause, &QPushButton::clicked, this, &LabWindow::togglePlay);
    connect(m_playPauseAction, &QAction::triggered, this, &LabWindow::togglePlay);
    connect(m_step, &QPushButton::clicked, this, [this] { m_engine->step(); });
    connect(m_stepAction, &QAction::triggered, this, [this] { m_engine->step(); });
    connect(m_stepBack, &QPushButton::clicked, this, [this] { m_engine->stepBack(); });
    connect(m_stepBackAction, &QAction::triggered, this, [this] { m_engine->stepBack(); });
    connect(m_restart, &QPushButton::clicked, this, [this] { m_engine->restart(); });
    connect(m_loop, &QCheckBox::toggled, this, [this](bool on) { m_engine->setLoop(on); });
    connect(m_asFast, &QCheckBox::toggled, this, [this](bool on) { m_engine->setAsFastAsPossible(on); });
    connect(m_record, &QPushButton::toggled, this, &LabWindow::toggleRecord);
    connect(m_capture, &QPushButton::clicked, this, &LabWindow::capture);
    connect(m_openLabFolder, &QPushButton::clicked, this, &LabWindow::openLabFolder);
    connect(m_chooseLabFolder, &QPushButton::clicked, this, &LabWindow::chooseLabFolder);
    connect(m_send, &QCheckBox::toggled, this, &LabWindow::toggleSend);
    connect(m_endpoint, &QLineEdit::editingFinished, this, &LabWindow::endpointChanged);
    connect(m_closeTimer, &QTimer::timeout, this, [this] {
        if (m_dependencies.engineMode == lab_core::Engine::Mode::Immediate) {
            m_engine->pump();
        }
        refreshStatus();
        if (m_engine->shutDown()) {
            m_closeTimer->stop();
            close();
        }
    });
    connect(m_cameraCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseCamera);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseFormat);
    connect(m_processorCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseProcessor);
    connect(m_showCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseShow);
    connect(m_overlaysCheck, &QCheckBox::toggled, this, [this](bool shown) {
        m_canvas->setOverlaysShown(shown);
        m_settings.setOverlays(shown);
    });
    connect(m_canvas, &ImageView::scaleChanged, this, &LabWindow::redrawForView);
    connect(m_canvas, &ImageView::panned, this, &LabWindow::redrawForView);
    connect(m_statusTimer, &QTimer::timeout, this, &LabWindow::refreshStatus);
}

void LabWindow::startReplay(lab_core::ReplayOpener open) {
    if (m_closing) {
        return; // the window would wait for this run too
    }
    stop(); // a camera running stops first
    beginRun();
    m_engine->startReplay(std::move(open));
}

// Results of an earlier run are dropped from here on: a new run's generation is at least one more.
void LabWindow::beginRun() {
    m_running = true;
    m_message.clear();
    m_minimumGeneration = m_engine->status().generation + 1;
    m_fitNext = true;
    updateStartStop();
}

void LabWindow::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;
    if (m_cameraRunning) {
        m_cameraRunning = false;
        m_session = 0;
        m_dependencies.cameras->stop();
    }
    m_engine->stop(); // the recording ends with the run
    if (m_record->isChecked()) {
        const QSignalBlocker blocker(m_record);
        m_record->setChecked(false);
    }
    viewChanged();
    m_current.reset();
    m_previous.reset();
    m_onScreen.reset();
    m_canvas->clear();
    m_placeholder->hide();
    m_results->clear();
    m_fitNext = true;
    updateStartStop();
    refreshStatus();
}

// Start while nothing runs and there is a camera; Stop while anything runs. Nothing starts while the window
// waits to close.
void LabWindow::updateStartStop() {
    m_startStop->setText(m_running ? tr("Stop") : tr("Start"));
    m_startStop->setEnabled(!m_closing && (m_running || currentCamera() != nullptr));
    m_openFile->setEnabled(!m_closing);
    m_openFolder->setEnabled(!m_closing);
}

// --- Cameras
// ------------------------------------------------------------------------------------------------

void LabWindow::fillCameras() {
    const CameraInfo* shown = currentCamera();
    const QByteArray wanted = shown ? shown->id : m_settings.cameraId();
    m_cameras = m_dependencies.cameras->cameras();
    {
        const QSignalBlocker blocker(m_cameraCombo);
        m_cameraCombo->clear();
        int index = 0;
        for (std::size_t i = 0; i < m_cameras.size(); ++i) {
            m_cameraCombo->addItem(m_cameras[i].name);
            if (m_cameras[i].id == wanted) {
                index = static_cast<int>(i);
            }
        }
        m_cameraCombo->setCurrentIndex(m_cameras.empty() ? -1 : index);
    }
    fillFormats();
    updateStartStop();
}

// The chosen camera's formats, once each, with the one chosen last time for it, else its default.
void LabWindow::fillFormats() {
    const QSignalBlocker blocker(m_formatCombo);
    m_formatCombo->clear();
    m_formats.clear();
    const CameraInfo* camera = currentCamera();
    if (camera == nullptr) {
        return;
    }
    m_formats = distinctFormats(camera->formats);
    for (const CameraFormat& format : m_formats) {
        m_formatCombo->addItem(format.label());
    }
    int index = m_formatCombo->findText(m_settings.formatFor(camera->id));
    if (index < 0) {
        const std::optional<CameraFormat> preferred = defaultFormat(m_formats);
        index = preferred ? m_formatCombo->findText(preferred->label()) : 0;
    }
    m_formatCombo->setCurrentIndex(std::max(index, 0));
}

void LabWindow::chooseCamera(int index) {
    if (index < 0) {
        return;
    }
    m_settings.setCameraId(currentCamera()->id);
    fillFormats();
    if (m_cameraRunning) {
        restartCamera();
    }
}

void LabWindow::chooseFormat(int index) {
    const CameraInfo* camera = currentCamera();
    if (index < 0 || camera == nullptr) {
        return;
    }
    m_settings.setFormatFor(camera->id, m_formatCombo->itemText(index));
    if (m_cameraRunning) {
        restartCamera();
    }
}

void LabWindow::startOrStop() {
    if (m_running) {
        stop();
    } else {
        startCamera();
    }
}

// Camera access first; the answer may come later, by when the window may be gone.
void LabWindow::startCamera() {
    if (currentCamera() == nullptr) {
        return;
    }
    m_message.clear();
    m_dependencies.cameras->requestPermission([window = QPointer<LabWindow>(this)](bool granted) {
        if (!window) {
            return;
        }
        if (granted) {
            window->startCameraGranted();
            return;
        }
#if defined(Q_OS_MACOS)
        window->m_message =
            tr("Camera access is off for the Document Lab: allow it in System Settings → Privacy & "
               "Security → Camera.");
#elif defined(Q_OS_WIN)
        window->m_message = tr("Camera access is off: allow it in Settings → Privacy & security → Camera.");
#else
        window->m_message = tr("Camera access was refused: allow it in the system's privacy settings.");
#endif
        window->refreshStatus();
    });
}

void LabWindow::startCameraGranted() {
    const CameraInfo* camera = currentCamera();
    if (camera == nullptr || m_closing) {
        return;
    }
    // A camera that lists no formats starts in its own default: no format matches an empty one.
    const std::optional<CameraFormat> format = currentFormat();
    stop(); // a replay running stops first
    beginRun();
    m_cameraRunning = true;
    m_geometry = {.frontCamera = camera->front};
    m_engine->startLive(
        (format ? camera->name + QStringLiteral(" · ") + format->label() : camera->name).toStdString());
    m_session = m_dependencies.cameras->start(camera->id, format.value_or(CameraFormat{}));
    refreshStatus();
}

// Another camera or format: a new run, and a new recording if one is wanted.
void LabWindow::restartCamera() {
    stop();
    startCamera();
}

// On the window's thread: only the frame's buffer is handed over; the processing thread converts it.
void LabWindow::cameraFrame(const QVideoFrame& frame, quint64 session) {
    if (!m_cameraRunning || session != m_session) {
        return; // a frame of a camera or session that is over
    }
    const std::chrono::nanoseconds at = now().time_since_epoch();
    m_engine->offer({[frame, geometry = m_geometry, at](std::string& error) {
        return toFrame(frame, geometry, at, error);
    }});
}

void LabWindow::cameraFailed(const QString& reason) {
    if (!m_cameraRunning) {
        return;
    }
    stop();
    m_message = reason;
    refreshStatus();
}

const CameraInfo* LabWindow::currentCamera() const {
    const int index = m_cameraCombo->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= m_cameras.size()) {
        return nullptr;
    }
    return &m_cameras[static_cast<std::size_t>(index)];
}

std::optional<CameraFormat> LabWindow::currentFormat() const {
    const int index = m_formatCombo->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= m_formats.size()) {
        return std::nullopt;
    }
    return m_formats[static_cast<std::size_t>(index)];
}

void LabWindow::pumpForTests() {
    m_engine->pump();
    takeResult();
    refreshStatus();
}

LabWindow::CanvasCounts LabWindow::canvasCountsForTests() const { return m_canvasCounts; }

void LabWindow::takeResult() {
    std::optional<lab_core::LabResult> result = m_engine->takeResult();
    if (!result || !m_running || result->generation < m_minimumGeneration) {
        return;
    }
    m_taken.push_back(now());
    m_previous = std::move(m_current);
    m_previousSequence = m_resultSequence;
    m_current = std::move(*result);
    ++m_resultSequence;
    updateShowEntries(*m_current);
    showResults(*m_current);
    display(m_fitNext ? ImageView::KeepView::No : ImageView::KeepView::IfSameSize);
    m_fitNext = false;
}

// Every picture a processor has made stays in the picker, so a choice survives frames that lack it.
void LabWindow::updateShowEntries(const lab_core::LabResult& result) {
    for (const lab_core::ProcessedImage& image : result.processed.images) {
        const QString name = QString::fromStdString(image.name);
        if (m_showCombo->findText(name) != -1) {
            continue;
        }
        m_showCombo->addItem(name);
        // The entry chosen last time, once it is there again.
        if (name == m_wantedShow) {
            m_showCombo->setCurrentText(name);
        }
    }
}

void LabWindow::showResults(const lab_core::LabResult& result) {
    m_results->clear();
    for (const std::string& finding : result.processed.findings) {
        m_results->addItem(QString::fromStdString(finding));
    }
    if (!result.processorError.empty()) {
        m_results->addItem(tr("error: %1").arg(QString::fromStdString(result.processorError)));
    }
}

// The picture Show names, with what is drawn over it; null when this result has none.
const cv::Mat* LabWindow::shownImage(visual_debug::ImageDescription& description) const {
    if (!m_current) {
        return nullptr;
    }
    const std::string entry = currentShow();
    if (entry == kFrameEntry.toStdString()) {
        description = m_current->frame.tags;
        description.overlays.insert(description.overlays.end(), m_current->processed.overlays.begin(),
                                    m_current->processed.overlays.end());
        return &m_current->frame.image;
    }
    for (const lab_core::ProcessedImage& image : m_current->processed.images) {
        if (image.name == entry) {
            description = image.description;
            return &image.image;
        }
    }
    return nullptr;
}

// Converted on the converter's thread at the size it will be drawn, as the viewer draws a live frame.
void LabWindow::display(ImageView::KeepView keep) {
    if (!m_current) {
        return;
    }
    visual_debug::ImageDescription description;
    const cv::Mat* image = shownImage(description);
    if (image == nullptr || image->empty()) {
        if (!m_placeholder->isVisible()) {
            viewChanged(); // nothing of the entry's earlier frames is drawn meanwhile
        }
        m_placeholder->setText(tr("%1 is not in this frame").arg(QString::fromStdString(currentShow())));
        m_placeholder->show();
        return;
    }
    m_placeholder->hide();
    const viewer_core::ImageTags tags = toImageTags(description);
    // While a fit converts, the zoom on screen is about to go: a picture cut for it would show only that part
    // of the fitted view.
    if (m_fitPending) {
        keep = ImageView::KeepView::No;
    }
    m_fitPending = keep == ImageView::KeepView::No;
    const LiveDrawing drawing = liveDrawing(*m_canvas, image->size(), keep);
    const std::optional<LiveJob> replaced =
        m_converter->submit({.viewCounter = m_viewCounter,
                             .channel = currentShow(),
                             .frameNumber = m_resultSequence,
                             .frame = *image,
                             .settings = viewer_core::defaultSettings(tags),
                             .tags = tags,
                             .area = drawing.area,
                             .drawnSize = drawing.size,
                             .showClipping = false,
                             .histogram = false,
                             .keep = keep});
    // Another result's picture replaced before it was drawn; a redraw of the same one loses nothing.
    if (replaced && replaced->frameNumber != m_resultSequence) {
        ++m_notDrawn;
        m_engine->reportNotDrawn(m_notDrawn);
    }
}

void LabWindow::applyPicture(LiveResult result) {
    ++m_canvasCounts.received;
    if (result.job.viewCounter != m_viewCounter) {
        return; // made for what was in view before
    }
    ++m_canvasCounts.applied;
    m_drawn.push_back(now());
    // The result this picture is of is now the one on screen: the newest, or the one before it.
    if (result.job.frameNumber == m_resultSequence && m_current) {
        m_onScreen = m_current;
    } else if (result.job.frameNumber == m_previousSequence && m_previous) {
        m_onScreen = m_previous;
    }
    const ImageView::KeepView keep = result.job.keep;
    const cv::Rect& area = result.job.area;
    m_canvas->setImage(result.picture, keep, result.imageSize, result.job.tags.overlays,
                       QRect(area.x, area.y, area.width, area.height));
    // The view fits only now: if the fitted view draws another area or size than the job assumed (the window
    // was resized meanwhile), draw it again for that.
    if (keep == ImageView::KeepView::No) {
        m_fitPending = false;
        const LiveDrawing fitted =
            liveDrawing(*m_canvas, cv::Size(result.imageSize.width(), result.imageSize.height()),
                        ImageView::KeepView::IfSameSize);
        if (fitted != LiveDrawing{area, cv::Size(result.picture.width(), result.picture.height())}) {
            display(ImageView::KeepView::IfSameSize);
        }
    }
}

// After a zoom, a fit, a resize or a pan, only when the newest picture no longer covers what is in view.
void LabWindow::redrawForView() {
    if (m_placeholder->isVisible()) {
        return;
    }
    visual_debug::ImageDescription description;
    const cv::Mat* image = shownImage(description);
    if (image == nullptr || image->empty()) {
        return;
    }
    if (needsRedraw(*m_canvas, image->size(), m_converter->newestDrawing())) {
        display(ImageView::KeepView::IfSameSize);
    }
}

// A picture still converting was for what was in view before; its result will be dropped.
void LabWindow::viewChanged() {
    ++m_viewCounter;
    m_fitPending = false;
}

void LabWindow::refreshStatus() {
    const lab_core::EngineStatus status = m_engine->status();
    const Clock::time_point at = now();
    for (std::deque<Clock::time_point>* times : {&m_taken, &m_drawn}) {
        while (!times->empty() && at - times->front() > std::chrono::seconds(1)) {
            times->pop_front();
        }
    }
    QStringList parts{stateText(status.state)};
    if (!status.source.empty()) {
        parts << QString::fromStdString(status.source);
    }
    if (status.frameNumber) {
        parts << (status.frameCount
                      ? tr("frame %1 of %2").arg(*status.frameNumber + 1).arg(*status.frameCount)
                      : tr("frame %1").arg(*status.frameNumber + 1));
    }
    parts << tr("%1 fps processed").arg(m_taken.size()) << tr("%1 fps shown").arg(m_drawn.size());
    if (m_current) {
        parts << tr("processing %1 ms")
                     .arg(std::chrono::duration_cast<std::chrono::milliseconds>(m_current->processingTime)
                              .count());
    }
    parts << tr("dropped %1").arg(status.droppedBeforeProcessing)
          << tr("not drawn %1").arg(m_notDrawn + status.resultsNotTaken)
          << tr("not sent %1").arg(status.sending ? status.sending->notSent : 0)
          << tr("not recorded %1").arg(status.recordingProgress ? status.recordingProgress->notRecorded : 0);
    if (status.recording && status.recordingProgress) {
        parts << tr("recording %1 frames")
                     .arg(status.recordingProgress->written + status.recordingProgress->queued);
    } else if (status.recordingProgress && !status.recordingProgress->finished) {
        parts << tr("writing recording: %1 of %2 frames")
                     .arg(status.recordingProgress->written)
                     .arg(status.recordingProgress->written + status.recordingProgress->queued);
    }
    if (status.recordingProgress && !status.recordingProgress->failure.empty()) {
        parts << tr("recording: %1").arg(QString::fromStdString(status.recordingProgress->failure));
    }
    // The engine stopped the recording (a restart, a loop, low disk space): Record is off again.
    if (m_record->isChecked() && m_recordingAsked && status.recording) {
        m_recordingAsked = false; // started: from now on its end shows here
    } else if (m_record->isChecked() && !m_recordingAsked && !status.recording) {
        const QSignalBlocker blocker(m_record);
        m_record->setChecked(false);
    }
    updateReplayControls(status);
    if (status.sending) {
        if (!m_sendingHost.isEmpty()) {
            parts << tr("sending to %1").arg(m_sendingHost);
        }
        if (status.sending->last) {
            parts << tr("viewer: %1").arg(qtString(visual_debug::toString(*status.sending->last)));
        }
    }
    if (status.busyFrame && status.busySince && at - *status.busySince >= kBusyShownAfter) {
        parts << tr("processing frame %1 for %2 s")
                     .arg(*status.busyFrame + 1)
                     .arg(std::chrono::duration_cast<std::chrono::seconds>(at - *status.busySince).count());
    }
    if (!status.message.empty()) {
        parts << QString::fromStdString(status.message);
    }
    if (!m_message.isEmpty()) {
        parts << m_message;
    }
    m_status->setText(parts.join(QStringLiteral(" · ")));
}

void LabWindow::chooseProcessor(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kProcessors.size()) {
        return;
    }
    const lab_core::ProcessorKind kind = kProcessors[static_cast<std::size_t>(index)];
    m_settings.setProcessor(qtString(lab_core::nameOf(kind)));
    m_engine->setProcessor(m_dependencies.makeProcessor(kind));
    forgetShowEntries();
}

// Show lists the pictures of the processor chosen: the earlier one's go, the frame shows until the choice
// made (still wanted) comes back with a picture of that name.
void LabWindow::forgetShowEntries() {
    const QString shown = m_showCombo->currentText();
    {
        const QSignalBlocker blocker(m_showCombo);
        while (m_showCombo->count() > 1) {
            m_showCombo->removeItem(m_showCombo->count() - 1);
        }
        m_showCombo->setCurrentIndex(0);
    }
    if (shown != kFrameEntry) {
        viewChanged();
        display(ImageView::KeepView::IfSameSize);
    }
}

void LabWindow::chooseShow(int index) {
    if (index < 0) {
        return;
    }
    m_wantedShow = m_showCombo->itemText(index);
    m_settings.setShow(m_wantedShow);
    viewChanged();
    display(ImageView::KeepView::IfSameSize);
}

std::string LabWindow::currentShow() const { return m_showCombo->currentText().toStdString(); }

Clock::time_point LabWindow::now() const { return m_dependencies.clock(); }

// --- Replay
// -------------------------------------------------------------------------------------------------

void LabWindow::openFile() {
    const QString path = m_dependencies.chooseFile
                             ? m_dependencies.chooseFile()
                             : QFileDialog::getOpenFileName(this, tr("Open a Video or an Image"));
    if (!path.isEmpty()) {
        openReplay(path);
    }
}

void LabWindow::openFolder() {
    const QString path =
        m_dependencies.chooseFolder
            ? m_dependencies.chooseFolder()
            : QFileDialog::getExistingDirectory(this, tr("Open a Folder of Images or a Recording"));
    if (!path.isEmpty()) {
        openReplay(path);
    }
}

// A recording, a folder of images, an image or a video, as frame_io tells them apart; opened on the engine's
// thread, so a slow disk never holds up the window.
void LabWindow::openReplay(const QString& path) {
    const std::filesystem::path file = toPath(path);
    std::error_code ignored;
    if (std::filesystem::is_directory(file, ignored)) {
        m_replayKind = std::filesystem::exists(file / "recording.json", ignored) ? "recording" : "images";
    } else {
        m_replayKind = frame_io::isImageFileName(file) ? "images" : "video";
    }
    startReplay([file](std::string& error) { return frame_io::openReplay(file, error); });
}

void LabWindow::togglePlay() {
    if (m_engine->status().state == lab_core::EngineState::Playing) {
        m_engine->pause();
    } else {
        m_engine->play();
    }
}

void LabWindow::updateReplayControls(const lab_core::EngineStatus& status) {
    const bool replay = m_running && !m_cameraRunning;
    const bool playing = status.state == lab_core::EngineState::Playing;
    m_playPause->setEnabled(replay);
    m_playPause->setText(playing ? tr("Pause") : tr("Play"));
    m_playPauseAction->setEnabled(replay);
    m_step->setEnabled(replay);
    m_stepAction->setEnabled(replay);
    m_stepBack->setEnabled(replay && status.canStepBack);
    m_stepBackAction->setEnabled(replay && status.canStepBack);
    m_restart->setEnabled(replay);
    m_record->setEnabled(m_running || m_record->isChecked());
    m_capture->setEnabled(m_onScreen.has_value());
}

// --- Recording and capture
// ----------------------------------------------------------------------------------

QString LabWindow::labFolder() const {
    const QString chosen = m_settings.labFolder();
    if (!chosen.isEmpty()) {
        return chosen;
    }
    if (!m_dependencies.labFolder.isEmpty()) {
        return m_dependencies.labFolder;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

void LabWindow::toggleRecord(bool on) {
    if (!on) {
        m_engine->stopRecording();
        refreshStatus();
        return;
    }
    QString failure;
    if (!startRecording(failure)) {
        const QSignalBlocker blocker(m_record);
        m_record->setChecked(false);
        m_message = failure;
        refreshStatus();
    }
}

// A new folder per recording, named by the local time, private to its owner, in the lab's folder.
bool LabWindow::startRecording(QString& failure) {
    const std::filesystem::path parent = toPath(labFolder());
    std::error_code made;
    std::filesystem::create_directories(parent, made);
    std::string error;
    const std::filesystem::path folder =
        frame_io::createUniqueFolder(parent, timeStamp().toStdString(), error);
    if (folder.empty() || !makePrivateFolder(fromPath(folder))) {
        failure = tr("Could not make a folder for the recording in %1: %2")
                      .arg(labFolder(), QString::fromStdString(error));
        return false;
    }
    frame_io::RecordingInfo info{
        .kind = m_cameraRunning ? "camera" : m_replayKind,
        .name = m_engine->status().source,
        .format = m_cameraRunning && currentFormat() ? currentFormat()->label().toStdString() : std::string(),
        .startedAt = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()};
    frame_io::RecordingWriter::Options options;
    options.mode = m_dependencies.recorderMode;
    options.beforeWrite = m_dependencies.recorderBeforeWrite;
    options.freeSpace = m_dependencies.freeSpace;
    std::unique_ptr<frame_io::RecordingWriter> writer =
        frame_io::RecordingWriter::start(folder, std::move(info), std::move(options), error);
    if (!writer) {
        failure = tr("Could not start the recording: %1").arg(QString::fromStdString(error));
        return false;
    }
    m_recordingAsked = true;
    m_engine->startRecording(std::move(writer));
    return true;
}

// The frame on screen at full resolution, with what the processor found in it, in the lab's Captures folder.
void LabWindow::capture() {
    if (!m_onScreen) {
        return;
    }
    const QString folder = QDir(labFolder()).filePath(QStringLiteral("Captures"));
    if (!makePrivateFolder(folder)) {
        m_message = tr("Could not make the Captures folder in %1.").arg(labFolder());
        refreshStatus();
        return;
    }
    const frame_io::Frame frame = m_onScreen->frame; // shares the pixels, which nothing changes
    const frame_io::CaptureInfo info{.source = m_engine->status().source,
                                     .findings = m_onScreen->processed.findings};
    const std::string baseName = timeStamp().toStdString();
    const std::filesystem::path path = toPath(folder);
    auto write = [path, baseName, frame, info]() -> std::pair<std::filesystem::path, std::string> {
        std::string error;
        std::filesystem::path written = frame_io::writeCapture(path, baseName, frame, info, error);
        return {std::move(written), std::move(error)};
    };
    if (m_dependencies.immediateCapture) {
        const auto [written, error] = write();
        captured(written, error);
        return;
    }
    QThreadPool::globalInstance()->start([write, window = QPointer<LabWindow>(this)] {
        auto [written, error] = write();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [window, written, error] {
                if (window) {
                    window->captured(written, error);
                }
            },
            Qt::QueuedConnection);
    });
}

void LabWindow::captured(const std::filesystem::path& file, const std::string& error) {
    m_message = file.empty() ? tr("Could not capture the frame: %1").arg(QString::fromStdString(error))
                             : tr("captured %1").arg(fromPath(file));
    refreshStatus();
}

void LabWindow::chooseLabFolder() {
    const QString folder =
        m_dependencies.chooseLabFolder
            ? m_dependencies.chooseLabFolder()
            : QFileDialog::getExistingDirectory(this, tr("Choose the Lab's Folder"), labFolder());
    if (folder.isEmpty()) {
        return;
    }
    m_settings.setLabFolder(folder);
    m_message = tr("Recordings may show real passports; avoid synced or shared folders.");
    refreshStatus();
}

void LabWindow::openLabFolder() {
    QDir().mkpath(labFolder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(labFolder()));
}

// --- Closing
// ------------------------------------------------------------------------------------------------

// The camera and the engine stop at once; the recording writes every queued frame first, its progress in the
// status bar, and the window closes when it has.
void LabWindow::closeEvent(QCloseEvent* event) {
    // Once: a command posted on every try would keep the engine from ever being shut down.
    if (!m_closing) {
        m_closing = true;
        stop();
        updateStartStop();
        m_engine->setSender(nullptr);
    }
    if (m_dependencies.engineMode == lab_core::Engine::Mode::Immediate) {
        m_engine->pump();
    }
    if (m_engine->shutDown()) {
        m_closeTimer->stop();
        event->accept();
        return;
    }
    event->ignore();
    refreshStatus();
    m_closeTimer->start(kClosePollMs);
}

// Space plays and pauses unless a button has the focus, which Space then presses.
bool LabWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const auto* pressed = qobject_cast<QAbstractButton*>(watched);
        if (pressed != nullptr && pressed->window() == this && key->key() == Qt::Key_Space &&
            key->modifiers() == Qt::NoModifier) {
            event->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// --- Send to viewer
// -----------------------------------------------------------------------------------------

void LabWindow::toggleSend(bool on) {
    m_settings.setSendToViewer(on);
    if (on) {
        startSending();
    } else {
        stopSending();
    }
}

// Another computer gets frames that may show real passports, unencrypted: only once the user has said yes to
// that endpoint, once per session. Refused, Send to viewer goes off again.
void LabWindow::startSending() {
    const QString endpoint = m_endpoint->text().trimmed();
    const std::string utf8 = endpoint.toStdString();
    const bool loopback = visual_debug::isLoopbackEndpoint(utf8);
    if (!loopback && !m_confirmedEndpoints.contains(endpoint)) {
        const QString question = tr("Frames may show real passports and travel unencrypted to %1. Send them?")
                                     .arg(hostOf(endpoint));
        const bool yes = m_dependencies.confirm ? m_dependencies.confirm(question)
                                                : QMessageBox::question(this, tr("Send to Another Computer"),
                                                                        question) == QMessageBox::Yes;
        if (!yes) {
            const QSignalBlocker blocker(m_send);
            m_send->setChecked(false);
            m_settings.setSendToViewer(false);
            stopSending();
            return;
        }
        m_confirmedEndpoints.insert(endpoint);
    }
    m_sendingHost = loopback ? QString() : hostOf(endpoint);
    m_engine->setSender(
        std::make_unique<lab_core::ResultSender>(m_dependencies.senderMode, m_dependencies.makeSender(utf8)));
    refreshStatus();
}

void LabWindow::stopSending() {
    m_sendingHost.clear();
    m_engine->setSender(nullptr);
    refreshStatus();
}

// A new endpoint gets a sender of its own, asked about as any other.
void LabWindow::endpointChanged() {
    const QString endpoint = m_endpoint->text().trimmed();
    if (endpoint == m_settings.endpoint()) {
        return;
    }
    m_settings.setEndpoint(endpoint);
    if (m_send->isChecked()) {
        startSending();
    }
}

} // namespace document_lab
