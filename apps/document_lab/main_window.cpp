#include "main_window.h"

#include "about.h"
#include "appearance.h"
#include "application_identity.h"
#include "camera_formats.h"
#include "image_tags_conversion.h"
#include "live_drawing.h"
#include "model_folder.h"
#include "outline_icons.h"
#include "private_files.h"
#include "scan_style.h"
#include "scanned_card.h"
#include "shades.h"

#include <doc_vision/guidance.h>
#include <frame_io/capture.h>
#include <frame_io/image_folder.h>
#include <viewer_core/image_tags.h>
#include <viewer_core/overlay.h>
#include <visual_debug/client.h>
#include <visual_debug/endpoint.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
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
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace document_lab {

namespace {

const QString kFrameEntry = QStringLiteral("Frame");

constexpr std::array kProcessors{lab_core::ProcessorKind::None, lab_core::ProcessorKind::Edges,
                                 lab_core::ProcessorKind::Outline, lab_core::ProcessorKind::Document};

constexpr int kResultsMinimumWidth = 120;
// Where a results row keeps its value's tone (lab_core::Finding::Tone), to colour it again for another
// appearance.
constexpr int kToneRole = Qt::UserRole + 1;

// A processor busy this long on one frame is named in the status bar.
constexpr auto kBusyShownAfter = std::chrono::seconds(1);
// A focus note stays in the status line this long after it came.
constexpr auto kFocusNoteShown = std::chrono::seconds(5);
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

viewer_core::Colour coreColour(const QColor& colour) {
    return {static_cast<std::uint8_t>(colour.red()), static_cast<std::uint8_t>(colour.green()),
            static_cast<std::uint8_t>(colour.blue())};
}

// The Document processor's outline as the canvas's guide (spec §3.5): a thick line on a halo, in the shown
// hint's colour, with no label; and A4's MRZ band as a dashed green guide, not drawn while the lab's focus
// searches (spec §3.2). toImageTags keeps one overlay per input and in order (image_tags_conversion.cpp), so
// the processor's i-th overlay follows the frame's own. The viewer and captures keep the plain "Document" and
// "MRZ band" polygons.
void markGuide(viewer_core::ImageTags& tags, const lab_core::LabResult& result) {
    const std::optional<lab_core::Guidance>& guidance = result.processed.guidance;
    if (!guidance) {
        return;
    }
    std::vector<viewer_core::Overlay> overlays = tags.overlayList();
    const auto polygonAt = [&](const std::optional<std::size_t>& processorIndex) -> viewer_core::Polygon* {
        if (!processorIndex || *processorIndex >= result.processed.overlays.size()) {
            return nullptr;
        }
        const std::size_t index = result.frame.tags.overlays.size() + *processorIndex;
        return index < overlays.size() ? std::get_if<viewer_core::Polygon>(&overlays[index]) : nullptr;
    };
    // Never green while the lab's focus searches: the lens sweeps through sharp positions (A3.3, spec §3.5).
    const bool searching = result.focus && result.focus->state == lab_core::FocusStatus::State::Searching;
    if (viewer_core::Polygon* outline = polygonAt(guidance->outlineOverlay)) {
        outline->style = viewer_core::Polygon::Style::Guide;
        outline->label.clear();
        outline->colour =
            coreColour(guidance->shownHint == doc_vision::Hint::Good && !searching ? kGuideGood : kGuideBad);
    }
    if (viewer_core::Polygon* band = polygonAt(guidance->bandOverlay)) {
        if (searching) {
            const auto index =
                static_cast<std::ptrdiff_t>(result.frame.tags.overlays.size() + *guidance->bandOverlay);
            overlays.erase(overlays.begin() + index);
        } else {
            band->style = viewer_core::Polygon::Style::GuideDashed;
            band->label.clear();
            band->colour = coreColour(kGuideGood);
        }
    }
    tags.overlays = viewer_core::makeOverlays(std::move(overlays));
}

QString timeStamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")); }

// What the picker says. Outline finds the largest bright quadrilateral, which a wall or a window can be too:
// it says it is simple. The settings keep lab_core's names.
QString processorLabel(lab_core::ProcessorKind kind) {
    switch (kind) {
    case lab_core::ProcessorKind::None:
        return LabWindow::tr("None");
    case lab_core::ProcessorKind::Edges:
        return LabWindow::tr("Edges");
    case lab_core::ProcessorKind::Outline:
        return LabWindow::tr("Outline (simple)");
    case lab_core::ProcessorKind::Document:
        return LabWindow::tr("Document");
    }
    return qtString(lab_core::nameOf(kind));
}

// A click beside a slider's handle jumps there, rather than a page at a time.
class JumpingSliderStyle : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* returnData) const override {
        if (hint == QStyle::SH_Slider_AbsoluteSetButtons) {
            return Qt::LeftButton;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

// A checkable button whose icon shows whether it is on (the accent colour), whatever the style draws.
QPushButton* makeToggle(const QString& text, qt_common::OutlineIcon icon, const QString& toolTip,
                        const char* name, QWidget* parent) {
    auto* toggle = new QPushButton(qt_common::makeOutlineIcon(icon), text, parent);
    toggle->setObjectName(QString::fromLatin1(name));
    toggle->setCheckable(true);
    toggle->setToolTip(toolTip);
    return toggle;
}

QPushButton* makeButton(const QString& text, const QIcon& icon, const QString& toolTip, const char* name,
                        QWidget* parent) {
    auto* button = new QPushButton(icon, text, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setToolTip(toolTip);
    return button;
}

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

QString formatPosition(std::chrono::nanoseconds at, std::chrono::nanoseconds length) {
    const bool hours = length >= std::chrono::hours(1);
    const auto text = [hours](std::chrono::nanoseconds time) {
        const long long seconds =
            std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(time).count());
        if (hours) {
            return QStringLiteral("%1:%2:%3")
                .arg(seconds / 3600)
                .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
                .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    };
    return text(at) + QStringLiteral(" / ") + text(length);
}

LabWindow::LabWindow(LabDependencies dependencies, QWidget* parent)
    : QMainWindow(parent), m_dependencies(std::move(dependencies)) {
    if (!m_dependencies.clock) {
        m_dependencies.clock = [] { return Clock::now(); };
    }
    if (!m_dependencies.makeProcessor) {
        const QString folder =
            m_dependencies.modelFolder.isEmpty() ? defaultModelFolder() : m_dependencies.modelFolder;
        m_dependencies.makeProcessor = [model = toPath(folder)](lab_core::ProcessorKind kind) {
            return lab_core::makeProcessor(kind, lab_core::ProcessorSettings{.modelFolder = model});
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

    // The link's hook comes on its thread: it only posts to this window's. No stream starts by itself: it can
    // show passports, so it starts on a click.
    if (!m_dependencies.makeLinkSender) {
        m_dependencies.makeLinkSender =
            [](const std::string& endpoint) -> std::unique_ptr<visual_debug::ImageSender> {
            return std::make_unique<visual_debug::Client>(visual_debug::ClientOptions{.endpoint = endpoint});
        };
    }
    m_link = std::make_unique<lab_core::ViewerLink>(
        m_dependencies.linkMode, m_dependencies.makeLinkSender,
        lab_core::ViewerLink::Hooks{.changed =
                                        [this] {
                                            QMetaObject::invokeMethod(
                                                this, [this] { refreshStatus(); }, Qt::QueuedConnection);
                                        },
                                    .now = m_dependencies.clock});
    applyEndpoint();

    chooseProcessor(m_processorCombo->currentIndex());

    // The window is the context: frames reach it on its own thread, and none after it is gone.
    CameraBackend* cameras = m_dependencies.cameras.get();
    connect(cameras, &CameraBackend::camerasChanged, this, &LabWindow::fillCameras);
    connect(cameras, &CameraBackend::frameArrived, this, &LabWindow::cameraFrame);
    connect(cameras, &CameraBackend::failed, this, &LabWindow::cameraFailed);
    fillCameras();

    qApp->installEventFilter(this);           // Space for the focused button, not Play
    m_canvas->setFocus(Qt::OtherFocusReason); // not the address field, the first that would take it
    refreshStatus();
    m_statusTimer->start(kStatusRefreshMs);
}

LabWindow::~LabWindow() {
    // Nothing a child sends while the window comes apart (the address field's editingFinished as it loses the
    // focus, a resize's scaleChanged) may reach this window once its destructor has run.
    for (QObject* child : findChildren<QObject*>()) {
        child->disconnect(this);
    }
    qApp->removeEventFilter(this);
    m_link.reset(); // joins: no hook comes after; closing waited for it to be idle
    // The camera first: no frame reaches a window whose engine is gone.
    m_dependencies.cameras->disconnect(this);
    m_dependencies.cameras->stop();
    m_statusTimer->stop();
    m_engine.reset();    // joins: no hook comes after
    m_converter.reset(); // waits for a conversion in progress
}

void LabWindow::buildUi() {
    setWindowTitle(tr("Document Lab"));
    // View: Results always, Appearance after it where Qt offers it (6.8 and newer); a hidden menu would leave
    // a hidden results panel no way back. Help, the About box.
    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));
    m_resultsAction = viewMenu->addAction(tr("Results"));
    m_resultsAction->setObjectName(QStringLiteral("resultsAction"));
    m_resultsAction->setCheckable(true);
    m_resultsAction->setChecked(m_settings.resultsShown());
    qt_common::addAppearanceMenu(viewMenu);
    qt_common::addHelpMenu(menuBar(),
                           QCoreApplication::translate("About", qt_common::kDocumentLabDescription));
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    using qt_common::OutlineIcon;

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
    m_startStop = makeButton(tr("Start"), qt_common::makeColouredIcon(OutlineIcon::Play, qt_common::kGo),
                             tr("Start the camera"), "startStopButton", central);
    m_startStop->setEnabled(false);
    sourceRow->addWidget(m_startStop);
    m_openFile = makeButton(tr("Open File…"), qt_common::makeOutlineIcon(OutlineIcon::Folder),
                            tr("Replay a video or an image"), "openFileButton", central);
    sourceRow->addWidget(m_openFile);
    m_openFolder = makeButton(tr("Open Folder…"), qt_common::makeOutlineIcon(OutlineIcon::Folder),
                              tr("Replay a folder of images or a recording"), "openFolderButton", central);
    sourceRow->addWidget(m_openFolder);
    sourceRow->addStretch();
    layout->addLayout(sourceRow);

    auto* replayRow = new QHBoxLayout;
    m_playPause = makeButton(tr("Play"), qt_common::makeColouredIcon(OutlineIcon::Play, qt_common::kGo),
                             tr("Play or pause the replay (Space)"), "playPauseButton", central);
    m_stepBack = makeButton(tr("Step Back"), qt_common::makeOutlineIcon(OutlineIcon::StepBack),
                            tr("The frame before (←)"), "stepBackButton", central);
    m_step = makeButton(tr("Step"), qt_common::makeOutlineIcon(OutlineIcon::StepForward),
                        tr("The next frame (→)"), "stepButton", central);
    m_restart = makeButton(tr("Restart"), qt_common::makeOutlineIcon(OutlineIcon::Restart),
                           tr("Back to the first frame"), "restartButton", central);
    m_loop = makeToggle(tr("Loop"), OutlineIcon::Loop, tr("Play again from the start at the end"),
                        "loopCheck", central);
    m_asFast = makeToggle(tr("As fast as possible"), OutlineIcon::Fast,
                          tr("Process every frame as soon as the last is done, not at the video's pace"),
                          "asFastCheck", central);
    m_back10 = makeButton(tr("10 s"), qt_common::makeOutlineIcon(OutlineIcon::Back10),
                          tr("Ten seconds back (Shift+←)"), "back10Button", central);
    m_forward10 = makeButton(tr("10 s"), qt_common::makeOutlineIcon(OutlineIcon::Forward10),
                             tr("Ten seconds forward (Shift+→)"), "forward10Button", central);
    for (QWidget* control : std::initializer_list<QWidget*>{m_playPause, m_stepBack, m_step, m_back10,
                                                            m_forward10, m_restart, m_loop, m_asFast}) {
        replayRow->addWidget(control);
    }
    // Where the replay is: a click beside the handle jumps there, as a video player's does.
    m_slider = new QSlider(Qt::Horizontal, central);
    m_slider->setObjectName(QStringLiteral("positionSlider"));
    m_slider->setStyle(new JumpingSliderStyle);
    m_slider->style()->setParent(m_slider);
    m_slider->setToolTip(tr("Drag, or click, to go to another frame"));
    m_slider->hide();
    replayRow->addWidget(m_slider, 1);
    m_position = new QLabel(central);
    m_position->setObjectName(QStringLiteral("positionLabel"));
    replayRow->addWidget(m_position);
    layout->addLayout(replayRow);
    // Keys for the replay: Space, → and ←, Shift+→ and Shift+←, from anywhere in the window but a focused
    // button (eventFilter).
    m_playPauseAction = new QAction(tr("Play or Pause"), this);
    m_playPauseAction->setShortcut(QKeySequence(Qt::Key_Space));
    m_stepAction = new QAction(tr("Step"), this);
    m_stepAction->setShortcut(QKeySequence(Qt::Key_Right));
    m_stepBackAction = new QAction(tr("Step Back"), this);
    m_stepBackAction->setShortcut(QKeySequence(Qt::Key_Left));
    m_back10Action = new QAction(tr("Ten Seconds Back"), this);
    m_back10Action->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Left));
    m_forward10Action = new QAction(tr("Ten Seconds Forward"), this);
    m_forward10Action->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Right));
    addActions({m_playPauseAction, m_stepAction, m_stepBackAction, m_back10Action, m_forward10Action});
    // F focuses the camera on the centre of what the canvas shows (A3.3).
    m_focusAction = new QAction(tr("Focus on the Centre"), this);
    m_focusAction->setObjectName(QStringLiteral("focusAction"));
    m_focusAction->setShortcut(QKeySequence(Qt::Key_F));
    m_focusAction->setEnabled(false); // during a camera run only
    addAction(m_focusAction);

    auto* processingRow = new QHBoxLayout;
    processingRow->addWidget(new QLabel(tr("Processor:"), central));
    m_processorCombo = new QComboBox(central);
    m_processorCombo->setObjectName(QStringLiteral("processorCombo"));
    int remembered = 0;
    for (const lab_core::ProcessorKind kind : kProcessors) {
        if (qtString(lab_core::nameOf(kind)) == m_settings.processor()) {
            remembered = m_processorCombo->count();
        }
        m_processorCombo->addItem(processorLabel(kind), static_cast<int>(kind));
    }
    m_processorCombo->setCurrentIndex(remembered);
    processingRow->addWidget(m_processorCombo);
    processingRow->addWidget(new QLabel(tr("Show:"), central));
    m_showCombo = new QComboBox(central);
    m_showCombo->setObjectName(QStringLiteral("showCombo"));
    m_showCombo->addItem(kFrameEntry);
    m_showCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_wantedShow = m_settings.show();
    processingRow->addWidget(m_showCombo);
    m_overlaysCheck =
        makeToggle(tr("Overlays"), OutlineIcon::Overlays,
                   tr("Draw what the processor found over the picture"), "overlaysCheck", central);
    m_overlaysCheck->setChecked(m_settings.overlays());
    processingRow->addWidget(m_overlaysCheck);
    processingRow->addStretch();
    m_record = makeButton(tr("Record"), qt_common::makeColouredIcon(OutlineIcon::Record, qt_common::kHalt),
                          tr("Record the frames processed from now on into the lab's folder"), "recordButton",
                          central);
    m_record->setCheckable(true);
    processingRow->addWidget(m_record);
    m_capture = makeButton(tr("Capture"), qt_common::makeOutlineIcon(OutlineIcon::Camera),
                           tr("Keep the frame on screen at full resolution, with what was found in it"),
                           "captureButton", central);
    processingRow->addWidget(m_capture);
    m_openLabFolder = makeButton(tr("Open Lab Folder"), qt_common::makeOutlineIcon(OutlineIcon::Folder),
                                 tr("Show the recordings and captures"), "openLabFolderButton", central);
    processingRow->addWidget(m_openLabFolder);
    m_chooseLabFolder =
        makeButton(tr("Choose Folder…"), QIcon(), tr("Choose where recordings and captures go"),
                   "chooseLabFolderButton", central);
    processingRow->addWidget(m_chooseLabFolder);
    layout->addLayout(processingRow);

    // The viewer: its address (about 22 characters), whether it is there, the stream's ▶ ⏸ ■, Send Frame.
    auto* viewerRow = new QHBoxLayout;
    viewerRow->addWidget(new QLabel(tr("Viewer:"), central));
    m_endpoint = new QLineEdit(m_settings.endpoint(), central);
    m_endpoint->setObjectName(QStringLiteral("endpointEdit"));
    m_endpoint->setPlaceholderText(QString::fromStdString(visual_debug::defaultEndpoint()));
    m_endpoint->setToolTip(
        tr("The viewer's address; another computer's is asked about before anything goes"));
    m_endpoint->setFixedWidth(
        m_endpoint->fontMetrics().horizontalAdvance(QStringLiteral("tcp://127.0.0.1:65535")) + 24);
    viewerRow->addWidget(m_endpoint);
    m_viewerIndicator = new QLabel(central);
    m_viewerIndicator->setObjectName(QStringLiteral("viewerIndicator"));
    m_viewerIndicator->setFixedSize(16, 16);
    viewerRow->addWidget(m_viewerIndicator);
    m_viewerState = new QLabel(central);
    m_viewerState->setObjectName(QStringLiteral("viewerStateLabel"));
    viewerRow->addWidget(m_viewerState);
    m_streamButton =
        makeButton(tr("Stream"), qt_common::makeColouredIcon(OutlineIcon::Play, qt_common::kGo),
                   tr("Send every frame to the viewer as it is processed, or go on after a pause"),
                   "streamButton", central);
    viewerRow->addWidget(m_streamButton);
    m_pauseStream = makeButton(QString(), qt_common::makeOutlineIcon(OutlineIcon::Pause),
                               tr("Pause the stream: the viewer keeps showing the last frame"),
                               "pauseStreamButton", central);
    viewerRow->addWidget(m_pauseStream);
    m_stopStream = makeButton(QString(), qt_common::makeColouredIcon(OutlineIcon::Stop, qt_common::kHalt),
                              tr("Stop the stream, and end the lab's channels in the viewer"),
                              "stopStreamButton", central);
    viewerRow->addWidget(m_stopStream);
    m_sendFrame =
        makeButton(tr("Send Frame"), qt_common::makeOutlineIcon(OutlineIcon::Send),
                   tr("Send the picture on screen into the viewer's history"), "sendFrameButton", central);
    viewerRow->addWidget(m_sendFrame);
    viewerRow->addStretch();
    layout->addLayout(viewerRow);

    // The canvas, with the placeholder over it in the same cell, beside the results.
    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setObjectName(QStringLiteral("resultsSplitter"));
    QSplitter* splitter = m_splitter;
    auto* canvasArea = new QWidget(splitter);
    auto* canvasLayout = new QGridLayout(canvasArea);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    m_canvas = new ImageView(canvasArea);
    m_canvas->setObjectName(QStringLiteral("canvas"));
    // The keyboard's place, so Space, ← and → reach the replay rather than a text field.
    m_canvas->setFocusPolicy(Qt::StrongFocus);
    m_canvas->setRegionsEnabled(false);
    m_canvas->setOverlaysShown(m_overlaysCheck->isChecked());
    m_canvas->setEmptyText(tr("Start a camera, or open a video, an image or a folder of images."));
    connect(m_canvas, &ImageView::clicked, this, &LabWindow::canvasClicked);
    connect(m_focusAction, &QAction::triggered, this, &LabWindow::focusOnTheCentre);
    canvasLayout->addWidget(m_canvas, 0, 0);
    m_placeholder = new QLabel(canvasArea);
    m_placeholder->setObjectName(QStringLiteral("placeholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setAutoFillBackground(true); // covers the picture of an earlier frame
    m_placeholder->hide();
    canvasLayout->addWidget(m_placeholder, 0, 0);
    // The Document processor's banner and chips, in the same cell after the placeholder and raised, so it
    // never covers them; each carries its gap from the canvas's edge as transparent margin.
    m_banner = new ScanBanner(canvasArea);
    m_banner->setObjectName(QStringLiteral("scanBanner"));
    canvasLayout->addWidget(m_banner, 0, 0, Qt::AlignTop | Qt::AlignHCenter);
    m_chips = new GateChips(canvasArea);
    m_chips->setObjectName(QStringLiteral("gateChips"));
    canvasLayout->addWidget(m_chips, 0, 0, Qt::AlignBottom | Qt::AlignHCenter);
    m_banner->raise();
    m_chips->raise();
    // A4's Scanned: the card over the dimmed scan, above everything else in the cell.
    m_scannedCard = new ScannedCard(canvasArea);
    m_scannedCard->setObjectName(QStringLiteral("scannedCard"));
    canvasLayout->addWidget(m_scannedCard, 0, 0);
    m_scannedCard->hide();
    m_scannedCard->raise();
    connect(m_scannedCard, &ScannedCard::skipChip, this, [this] { applyScanEvent(ScanEvent::SkipChip); });
    connect(m_scannedCard, &ScannedCard::scanAgain, this, [this] { applyScanEvent(ScanEvent::ScanAgain); });
    // Findings as rows of a name and a value; the panel keeps its width as the window grows, the canvas takes
    // the rest.
    m_results = new QTreeWidget(splitter);
    m_results->setObjectName(QStringLiteral("resultsList"));
    m_results->setColumnCount(2);
    m_results->setHeaderHidden(true);
    m_results->setRootIsDecorated(false);
    m_results->setMinimumWidth(kResultsMinimumWidth);
    splitter->addWidget(canvasArea);
    splitter->addWidget(m_results);
    // The panel can be dragged shut, as View → Results shuts it; its edge stays, to drag it open again. The
    // canvas cannot.
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, true);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    const int panel = m_resultsAction->isChecked() ? m_settings.resultsWidth() : 0;
    splitter->setSizes({std::max(1, width() - panel), panel});
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
    connect(m_back10, &QPushButton::clicked, this, [this] { seekSeconds(-10); });
    connect(m_back10Action, &QAction::triggered, this, [this] { seekSeconds(-10); });
    connect(m_forward10, &QPushButton::clicked, this, [this] { seekSeconds(10); });
    connect(m_forward10Action, &QAction::triggered, this, [this] { seekSeconds(10); });
    // While dragged, only the time follows; let go, or a click or a key, and it seeks.
    connect(m_slider, &QSlider::valueChanged, this, &LabWindow::sliderChanged);
    connect(m_slider, &QSlider::sliderReleased, this, [this] { sliderChanged(m_slider->value()); });
    connect(m_loop, &QPushButton::toggled, this, [this](bool on) { m_engine->setLoop(on); });
    connect(m_asFast, &QPushButton::toggled, this, [this](bool on) { m_engine->setAsFastAsPossible(on); });
    connect(m_resultsAction, &QAction::toggled, this, &LabWindow::showResultsPanel);
    // The width the user dragged the panel to, not one squeezed by a narrow window or a hidden panel.
    // Dragged shut, the panel is hidden as View → Results hides it; dragged open, shown. The width kept is
    // one above the panel's least, which a drag towards shut passes through on its way.
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        const int panel = m_splitter->sizes().value(1);
        if (panel > kResultsMinimumWidth) {
            m_settings.setResultsWidth(panel);
        }
        const bool shown = panel > 0;
        if (shown != m_resultsAction->isChecked()) {
            const QSignalBlocker blocker(m_resultsAction);
            m_resultsAction->setChecked(shown);
            m_settings.setResultsShown(shown);
        }
    });
    connect(m_record, &QPushButton::toggled, this, &LabWindow::toggleRecord);
    connect(m_capture, &QPushButton::clicked, this, &LabWindow::capture);
    connect(m_openLabFolder, &QPushButton::clicked, this, &LabWindow::openLabFolder);
    connect(m_chooseLabFolder, &QPushButton::clicked, this, &LabWindow::chooseLabFolder);
    connect(m_streamButton, &QPushButton::clicked, this, &LabWindow::startStream);
    connect(m_pauseStream, &QPushButton::clicked, this, &LabWindow::pauseStream);
    connect(m_stopStream, &QPushButton::clicked, this, &LabWindow::stopStream);
    connect(m_sendFrame, &QPushButton::clicked, this, &LabWindow::sendFrame);
    connect(m_endpoint, &QLineEdit::editingFinished, this, &LabWindow::endpointChanged);
    connect(m_closeTimer, &QTimer::timeout, this, [this] {
        if (m_dependencies.engineMode == lab_core::Engine::Mode::Immediate) {
            m_engine->pump();
        }
        if (m_dependencies.linkMode == lab_core::ViewerLink::Mode::Immediate) {
            m_link->pump();
        }
        refreshStatus();
        if (m_engine->shutDown() && m_link->idle()) {
            m_closeTimer->stop();
            close();
        }
    });
    connect(m_cameraCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseCamera);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseFormat);
    connect(m_processorCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseProcessor);
    connect(m_showCombo, &QComboBox::currentIndexChanged, this, &LabWindow::chooseShow);
    connect(m_overlaysCheck, &QPushButton::toggled, this, [this](bool shown) {
        m_canvas->setOverlaysShown(shown);
        m_settings.setOverlays(shown);
    });
    connect(m_canvas, &ImageView::scaleChanged, this, &LabWindow::redrawForView);
    connect(m_canvas, &ImageView::panned, this, &LabWindow::redrawForView);
    connect(m_statusTimer, &QTimer::timeout, this, &LabWindow::refreshStatus);
}

// Shut to nothing, its edge left at the window's side to drag it open; shown again at the width it had.
void LabWindow::showResultsPanel(bool shown) {
    m_settings.setResultsShown(shown);
    const QList<int> sizes = m_splitter->sizes();
    const int total = sizes.value(0) + sizes.value(1);
    const int panel = shown ? m_settings.resultsWidth() : 0;
    m_splitter->setSizes({std::max(1, total - panel), panel});
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
    m_focusNote.clear(); // a new run's controller numbers its notes from 1 again
    m_focusNoteNumber = 0;
    m_windowState.reset();
    // Space now plays and pauses this run, not the button that started it.
    m_canvas->setFocus(Qt::OtherFocusReason);
    m_minimumGeneration = m_engine->status().generation + 1;
    m_scanFrom = m_resultSequence + 1;
    m_fitNext = true;
    updateStartStop();
    updateScan(); // the first hint from the moment the run starts, before its first result
}

void LabWindow::stop() {
    dropScan(false);
    endRun(false);
}

void LabWindow::endRun(bool keepScan) {
    if (!m_running) {
        return;
    }
    m_running = false;
    if (m_cameraRunning) {
        m_cameraRunning = false;
        m_focusAction->setEnabled(false);
        m_session = 0;
        m_dependencies.cameras->stop();
    }
    m_cameraPaused = false;
    m_engine->stop(); // the recording ends with the run
    if (m_record->isChecked()) {
        const QSignalBlocker blocker(m_record);
        m_record->setChecked(false);
    }
    viewChanged();
    m_current.reset();
    m_previous.reset();
    m_onScreen.reset();
    m_fitNext = true;
    if (keepScan) {
        display(ImageView::KeepView::IfSameSize); // a picture of it still converting was dropped
    } else {
        m_handledScan.reset(); // a new run's processor starts afresh
        m_canvas->clear();
        m_placeholder->hide();
        m_results->clear();
    }
    updateStartStop();
    updateScan();
    refreshStatus();
}

// Start while nothing runs and there is a camera; Stop while anything runs. Nothing starts while the window
// waits to close.
void LabWindow::updateStartStop() {
    m_startStop->setText(m_running ? tr("Stop") : tr("Start"));
    m_startStop->setIcon(m_running
                             ? qt_common::makeColouredIcon(qt_common::OutlineIcon::Stop, qt_common::kHalt)
                             : qt_common::makeColouredIcon(qt_common::OutlineIcon::Play, qt_common::kGo));
    m_startStop->setToolTip(m_running ? tr("Stop the camera or the replay") : tr("Start the camera"));
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
    updateScan();
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
    } else {
        dropScan(false); // a scan kept after its camera failed ends too (spec §3.1)
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
    } else {
        dropScan(false);
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
        window->m_windowState = WindowState::AccessOff;
        window->updateScan();
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
    m_focusAction->setEnabled(true);
    m_firstFrameAt.reset();
    m_geometry = {.frontCamera = camera->front};
    m_engine->startLive(
        (format ? camera->name + QStringLiteral(" · ") + format->label() : camera->name).toStdString(),
        m_dependencies.makeFocusControl ? m_dependencies.makeFocusControl(camera->id) : nullptr);
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
    if (m_cameraPaused) {
        return; // a scan shows (spec §3.1, D3): the session stays open, its frames go nowhere
    }
    // From the run's first frame, as a replay's times are.
    const Clock::time_point arrived = now();
    if (!m_firstFrameAt) {
        m_firstFrameAt = arrived;
    }
    const auto at = std::chrono::duration_cast<std::chrono::nanoseconds>(arrived - *m_firstFrameAt);
    m_engine->offer({[frame, geometry = m_geometry, at](std::string& error) {
        return toFrame(frame, geometry, at, error);
    }});
}

// A click on the canvas focuses there while a camera runs and the Frame shows: the canvas's image pixels are
// then the frame's, which the engine's focusAt takes as shares.
void LabWindow::canvasClicked(QPointF imagePoint) {
    if (!m_cameraRunning || scanShowing()) {
        return;
    }
    if (currentShow() != kFrameEntry.toStdString()) {
        noteFocus(tr("Show the Frame to focus by clicking"));
        refreshStatus();
        return;
    }
    focusAt(imagePoint);
}

void LabWindow::focusOnTheCentre() {
    if (!m_cameraRunning || scanShowing()) {
        return;
    }
    if (currentShow() != kFrameEntry.toStdString()) {
        noteFocus(tr("Show the Frame to focus by clicking"));
        refreshStatus();
        return;
    }
    const QRect visible = m_canvas->visibleArea();
    if (!visible.isEmpty()) {
        focusAt(QRectF(visible).center());
    }
}

void LabWindow::focusAt(QPointF imagePoint) {
    if (!m_current || m_current->frame.image.empty()) {
        return;
    }
    m_engine->focusAt(imagePoint.x() / m_current->frame.image.cols,
                      imagePoint.y() / m_current->frame.image.rows);
}

void LabWindow::noteFocus(const QString& note) {
    m_focusNote = note;
    m_focusNoteAt = now();
}

void LabWindow::cameraFailed(const QString& reason) {
    if (!m_cameraRunning) {
        return;
    }
    // Spec §3.1: the card and Review work without a camera, so a scan showing stays; going on starts it
    // again.
    const bool keepScan = scanShowing();
    endRun(keepScan);
    m_cameraLost = keepScan;
    m_message = reason;
    m_windowState = WindowState::CameraStopped;
    m_windowStateDetail = reason;
    updateScan();
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
    m_link->pump();
    takeResult();
    refreshStatus();
}

void LabWindow::pumpEngineForTests() { m_engine->pump(); }

LabWindow::CanvasCounts LabWindow::canvasCountsForTests() const { return m_canvasCounts; }

void LabWindow::takeResult() {
    std::optional<lab_core::LabResult> result = m_engine->takeResult();
    if (!result || !m_running || result->generation < m_minimumGeneration) {
        return;
    }
    m_taken.push_back(now());
    // Spec §3.1: results that come while Scanned or Review shows (offered before the pause) are taken, so the
    // engine's slot keeps moving, but change nothing on screen.
    if (scanShowing()) {
        return;
    }
    m_previous = std::move(m_current);
    m_previousSequence = m_resultSequence;
    m_current = std::move(*result);
    ++m_resultSequence;
    if (const std::optional<lab_core::FocusStatus>& focus = m_current->focus;
        focus && focus->noteNumber != m_focusNoteNumber) {
        m_focusNoteNumber = focus->noteNumber;
        if (!focus->note.empty()) {
            noteFocus(QString::fromStdString(focus->note));
        }
    }
    // An accept, under whatever hint shows (spec §3.1): every result from it on carries the scan, so one
    // replaced in the engine's slot is not missed; the one handled last never comes back.
    if (const std::optional<lab_core::Guidance>& guidance = m_current->processed.guidance;
        documentChosen() && m_resultSequence >= m_scanFrom && guidance && guidance->scan &&
        guidance->scan != m_handledScan) {
        accept(guidance->scan);
    }
    updateShowEntries(*m_current);
    showResults(*m_current);
    display(m_fitNext ? ImageView::KeepView::No : ImageView::KeepView::IfSameSize);
    m_fitNext = false;
    updateScan();
    // The frame's number, the slider and the time follow each result, not only the refresh twice a second.
    refreshStatus();
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

// A finding as a row of its name and its value, the value coloured by its tone; the tone is kept in the row,
// so a change of appearance can colour it again.
void LabWindow::showResults(const lab_core::LabResult& result) {
    m_results->clear();
    for (const lab_core::Finding& finding : result.processed.findings) {
        auto* row = new QTreeWidgetItem(
            QStringList{QString::fromStdString(finding.name), QString::fromStdString(finding.value)});
        row->setData(1, kToneRole, static_cast<int>(finding.tone));
        m_results->addTopLevelItem(row);
    }
    if (!result.processorError.empty()) {
        auto* row =
            new QTreeWidgetItem(QStringList{tr("error"), QString::fromStdString(result.processorError)});
        row->setData(1, kToneRole, static_cast<int>(lab_core::Finding::Tone::Bad));
        m_results->addTopLevelItem(row);
    }
    colourResults();
    m_results->resizeColumnToContents(0);
}

// Each row's value in its tone's colour for the appearance the application now has.
void LabWindow::colourResults() {
    const bool dark = qt_common::isDarkAppearance(QApplication::palette());
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = m_results->topLevelItem(i);
        const QColor colour =
            toneColour(static_cast<lab_core::Finding::Tone>(row->data(1, kToneRole).toInt()), dark);
        row->setForeground(1, colour.isValid() ? QBrush(colour) : QBrush());
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
    const cv::Mat* image = nullptr;
    viewer_core::ImageTags tags;
    if (scanShowing()) {
        // The scan's frame, whatever the newest result holds (spec §3.1); Scanned draws nothing over it.
        if (!m_scan) {
            return;
        }
        image = &m_scan->frame.image;
        tags = toImageTags(visual_debug::ImageDescription{});
    } else {
        if (!m_current) {
            return;
        }
        visual_debug::ImageDescription description;
        image = shownImage(description);
        if (image == nullptr || image->empty()) {
            if (!m_placeholder->isVisible()) {
                viewChanged(); // nothing of the entry's earlier frames is drawn meanwhile
            }
            m_placeholder->setText(tr("%1 is not in this frame").arg(QString::fromStdString(currentShow())));
            m_placeholder->show();
            return;
        }
        tags = toImageTags(description);
        if (currentShow() == kFrameEntry.toStdString()) {
            markGuide(tags, *m_current);
        }
    }
    if (image->empty()) {
        return;
    }
    m_placeholder->hide();
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
        m_onScreenSequence = m_resultSequence;
    } else if (result.job.frameNumber == m_previousSequence && m_previous) {
        m_onScreen = m_previous;
        m_onScreenSequence = m_previousSequence;
    }
    const ImageView::KeepView keep = result.job.keep;
    const cv::Rect& area = result.job.area;
    m_canvas->setImage(result.picture, keep, result.imageSize, result.job.tags.overlays,
                       QRect(area.x, area.y, area.width, area.height));
    updateScan(); // the banner and chips change with the picture and its outline
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
    const cv::Mat* image =
        scanShowing() ? (m_scan ? &m_scan->frame.image : nullptr) : shownImage(description);
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
    // A camera paused for a scan says so; a replay's own word is the engine's, "Paused" (spec §3.1).
    QStringList parts{scanShowing() && m_cameraRunning ? tr("Camera paused") : stateText(status.state)};
    // The news next, before the source and the counters: a window narrower than the whole line cuts its end.
    if (documentChosen() && scanShowing()) {
        parts << (m_scanState == ScanState::Scanned ? tr("State: scanned, waiting for the chip")
                                                    : tr("State: review"));
    } else if (documentChosen() && m_running) {
        const lab_core::Guidance* guidance = drawnGuidance();
        if (guidance == nullptr || guidance->unavailable == lab_core::Guidance::Unavailable::None) {
            parts << tr("State: %1").arg(scanStateWord(guidance));
            if (guidance != nullptr && guidance->reading && !guidance->reading->available) {
                parts << tr("MRZ reader: %1").arg(QString::fromStdString(guidance->reading->reason));
            }
        } else {
            parts << QString::fromStdString(guidance->reason);
        }
    }
    if (!m_focusNote.isEmpty() && at - m_focusNoteAt < kFocusNoteShown) {
        parts << m_focusNote;
    }
    if (!status.message.empty()) {
        parts << QString::fromStdString(status.message);
    }
    if (!m_message.isEmpty()) {
        parts << m_message;
    }
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
    updateViewerRow(status);
    if (status.sending) {
        if (!m_sendingHost.isEmpty()) {
            parts << tr("sending to %1").arg(m_sendingHost);
        }
        if (status.sending->last) {
            parts << tr("viewer: %1").arg(qtString(visual_debug::toString(*status.sending->last)));
        }
        if (status.sending->paused) {
            parts << tr("stream paused");
        }
    }
    const lab_core::ViewerLink::State link = m_link->state();
    if (!link.lastStillName.empty() && link.lastStill) {
        parts << (*link.lastStill == visual_debug::SendResult::Delivered
                      ? tr("sent %1 to the viewer").arg(QString::fromStdString(link.lastStillName))
                      : tr("%1 not sent: %2")
                            .arg(QString::fromStdString(link.lastStillName),
                                 qtString(visual_debug::toString(*link.lastStill))));
    }
    if (status.seeking) {
        parts << tr("seeking…");
    }
    if (status.busyFrame && status.busySince && at - *status.busySince >= kBusyShownAfter) {
        parts << tr("processing frame %1 for %2 s")
                     .arg(*status.busyFrame + 1)
                     .arg(std::chrono::duration_cast<std::chrono::seconds>(at - *status.busySince).count());
    }
    m_status->setText(parts.join(QStringLiteral(" · ")));
}

void LabWindow::chooseProcessor(int index) {
    if (index < 0) {
        return;
    }
    dropScan(true); // another processor ends a scan as Discard does; the run goes on (spec §3.1)
    const auto kind = static_cast<lab_core::ProcessorKind>(m_processorCombo->itemData(index).toInt());
    m_settings.setProcessor(qtString(lab_core::nameOf(kind)));
    m_engine->setProcessor(m_dependencies.makeProcessor(kind));
    // The guidance on screen is the earlier processor's: the new one's first hint shows until it has a
    // result.
    m_scanFrom = m_resultSequence + 1;
    forgetShowEntries();
    updateScan();
    refreshStatus(); // the Document processor's state comes and goes with it
}

bool LabWindow::documentChosen() const {
    return static_cast<lab_core::ProcessorKind>(m_processorCombo->currentData().toInt()) ==
           lab_core::ProcessorKind::Document;
}

const lab_core::LabResult* LabWindow::drawnResult() const {
    if (!m_running) {
        return nullptr;
    }
    // While the placeholder shows, no picture keeps the banner in step with anything: the newest result does.
    const bool placeholder = !m_placeholder->isHidden();
    const std::optional<lab_core::LabResult>& result = placeholder ? m_current : m_onScreen;
    const std::uint64_t sequence = placeholder ? m_resultSequence : m_onScreenSequence;
    if (!result || sequence < m_scanFrom || !result->processed.guidance) {
        return nullptr;
    }
    return &*result;
}

const lab_core::Guidance* LabWindow::drawnGuidance() const {
    const lab_core::LabResult* result = drawnResult();
    return result != nullptr ? &*result->processed.guidance : nullptr;
}

void LabWindow::updateScan() {
    if (!documentChosen()) {
        m_banner->setContent(std::nullopt);
        m_chips->setStates({});
        m_canvas->setEmptyText(
            m_cameras.empty()
                ? tr("No camera found. Connect one, or open a video, an image or a folder of images.")
                : tr("Start a camera, or open a video, an image or a folder of images."));
        return;
    }
    m_canvas->setEmptyText(QString()); // the banner is the one message
    if (scanShowing()) {               // the card or Review says what there is to say
        m_banner->setContent(std::nullopt);
        m_banner->setProgress(std::nullopt);
        m_chips->setStates({});
        return;
    }
    std::optional<BannerContent> content;
    std::optional<int> progress;
    doc_vision::ChipStates chips{};
    if (const lab_core::Guidance* guidance = drawnGuidance()) {
        if (guidance->unavailable != lab_core::Guidance::Unavailable::None) {
            content = bannerFor(guidance->unavailable == lab_core::Guidance::Unavailable::Runtime
                                    ? WindowState::RuntimeMissing
                                    : WindowState::ModelMissing);
        } else {
            // Spec §3.5: a search running shows "focusing" over every hint but NoPage, with Sharp ✗; "can't
            // focus" shows only while the shown hint is Blurred.
            const std::optional<lab_core::FocusStatus>& focus = drawnResult()->focus;
            chips = guidance->chips;
            if (focus && focus->state == lab_core::FocusStatus::State::Searching &&
                guidance->shownHint != doc_vision::Hint::NoPage) {
                content = bannerFor(FocusBanner::Focusing);
                chips.passed[static_cast<std::size_t>(doc_vision::Gate::Sharp)] = false;
            } else if (focus && focus->state == lab_core::FocusStatus::State::CannotFocus &&
                       guidance->shownHint == doc_vision::Hint::Blurred) {
                content = bannerFor(focus->afterSearch ? FocusBanner::CannotFocusThisClose
                                                       : FocusBanner::CannotMakeSharp);
            } else {
                content = bannerFor(guidance->shownHint);
                // A4's Reading (spec §3.2): hint 10 with the count and its bar, or why nothing is read.
                if (guidance->shownHint == doc_vision::Hint::Good && guidance->reading) {
                    if (!guidance->reading->available) {
                        content->smallLine = readerUnavailableLine();
                    } else if (oneFrameSource()) {
                        content->smallLine = readingNeedsMotion();
                    } else {
                        const int agreeing = std::clamp(guidance->reading->agreeing, 0, 3);
                        content->smallLine = readingLine(agreeing);
                        progress = agreeing;
                    }
                }
            }
        }
    } else if (m_running) {
        content = bannerFor(doc_vision::Hint::NoPage);
    } else if (m_windowState) {
        content = bannerFor(*m_windowState, m_windowStateDetail);
    } else {
        content = bannerFor(m_cameras.empty() ? WindowState::NoCamera : WindowState::Idle);
    }
    m_banner->setContent(std::move(content));
    m_banner->setProgress(progress);
    m_chips->setStates(chips);
}

bool LabWindow::oneFrameSource() const { return m_engine->status().frameCount == std::uint64_t{1}; }

// --- A4: the scan
// ------------------------------------------------------------------------------------------------

bool LabWindow::scanShowing() const { return m_scanState != ScanState::Guiding; }

// The accept pauses what feeds the engine and shows the card over the scan's frame (spec §3.1, D3): a
// camera's frames stop being handed over, its session open; a playing replay pauses.
void LabWindow::accept(std::shared_ptr<const lab_core::MrzScan> scan) {
    const std::optional<ScanStep> step = next(m_scanState, ScanEvent::Accepted);
    if (!step) {
        return;
    }
    m_handledScan = scan;
    m_scan = std::move(scan);
    m_scanState = step->to;
    if (m_cameraRunning) {
        m_cameraPaused = true;
    } else {
        m_replayWasPlaying = m_engine->status().state == lab_core::EngineState::Playing;
        if (m_replayWasPlaying) {
            m_engine->pause();
        }
    }
    showScanState();
}

void LabWindow::applyScanEvent(ScanEvent event) {
    const std::optional<ScanStep> step = next(m_scanState, event);
    if (!step) {
        return;
    }
    m_scanState = step->to;
    if (!step->keepScan) {
        m_scan.reset();
    }
    if (step->resume) {
        resumeScanning();
    }
    showScanState();
}

void LabWindow::resumeScanning() {
    if (m_cameraLost) {
        m_cameraLost = false;
        m_canvas->clear();
        m_results->clear();
        startCamera(); // fails as A3.1's window state says: "The camera stopped"
        return;
    }
    m_engine->resetProcessor();
    // The guidance on screen is of the scan's frames: the banner waits for a new result's.
    m_scanFrom = m_resultSequence + 1;
    m_cameraPaused = false;
    if (m_replayWasPlaying) {
        m_engine->play(); // only a replay the accept paused playing; a user's pause, or its end, stays
    }
    m_replayWasPlaying = false;
}

void LabWindow::dropScan(bool runContinues) {
    if (!scanShowing()) {
        return;
    }
    m_scanState = ScanState::Guiding;
    m_scan.reset();
    m_cameraPaused = false;
    if (runContinues && m_replayWasPlaying) {
        m_engine->play();
    }
    m_replayWasPlaying = false;
    m_scannedCard->hide();
    if (m_cameraLost || !m_running) { // its camera failed: nothing runs to draw over it
        m_cameraLost = false;
        m_canvas->clear();
        m_results->clear();
    }
    updateScan();
    refreshStatus();
}

void LabWindow::showScanState() {
    m_scannedCard->setVisible(m_scanState == ScanState::Scanned);
    viewChanged();
    display(ImageView::KeepView::IfSameSize);
    updateScan();
    refreshStatus();
}

QString LabWindow::scanStateWord(const lab_core::Guidance* guidance) const {
    if (guidance == nullptr) {
        return stateWord(doc_vision::Hint::NoPage);
    }
    // Hint 10 says "reading" while the reader reads; "good" when it cannot (spec §3.2).
    const bool reads = guidance->reading && guidance->reading->available && !oneFrameSource();
    if (guidance->shownHint == doc_vision::Hint::Good && reads) {
        return stateWord(doc_vision::Hint::NoMrzBand); // "reading"
    }
    return stateWord(guidance->shownHint);
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
    updateScan(); // the placeholder may have come or gone
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

void LabWindow::seekSeconds(int seconds) {
    const lab_core::EngineStatus status = m_engine->status();
    if (!m_running || m_cameraRunning || !status.canSeek || !status.frameRate) {
        return;
    }
    m_engine->seekBy(std::llround(seconds * *status.frameRate));
}

// Set from the status with its signals blocked, so only the user's moves arrive here.
void LabWindow::sliderChanged(int value) {
    if (m_slider->isSliderDown()) {
        updatePosition(m_engine->status()); // the time follows the drag
        return;
    }
    if (m_running && !m_cameraRunning && value >= 0) {
        m_engine->seek(static_cast<std::uint64_t>(value));
    }
}

// The slider for a replay that can seek and has frames to seek in; the handle on the frame on screen, or on a
// seek's target until its frame comes; the time of either, or of the frame dragged to, and the length.
void LabWindow::updatePosition(const lab_core::EngineStatus& status) {
    const bool replay = m_running && !m_cameraRunning;
    const bool seekable = replay && status.canSeek && status.frameRate && *status.frameRate > 0.0 &&
                          status.frameCount && *status.frameCount > 1;
    m_slider->setVisible(seekable);
    if (!seekable) {
        m_position->clear();
        return;
    }
    const double rate = *status.frameRate;
    const auto timeOf = [rate](double frames) {
        return std::chrono::nanoseconds(std::llround(frames * 1e9 / rate));
    };
    const std::chrono::nanoseconds length = timeOf(static_cast<double>(*status.frameCount));
    if (m_slider->isSliderDown()) {
        m_position->setText(formatPosition(timeOf(m_slider->value()), length));
        return;
    }
    const std::optional<std::uint64_t> shown = status.seeking ? status.seekTarget : status.frameNumber;
    {
        const QSignalBlocker blocker(m_slider);
        m_slider->setRange(0, static_cast<int>(std::min<std::uint64_t>(*status.frameCount - 1,
                                                                       std::numeric_limits<int>::max())));
        if (shown) {
            m_slider->setValue(
                static_cast<int>(std::min<std::uint64_t>(*shown, std::numeric_limits<int>::max())));
        }
    }
    std::chrono::nanoseconds at{};
    if (status.seeking && status.seekTarget) {
        at = timeOf(static_cast<double>(*status.seekTarget));
    } else if (m_current) {
        at = m_current->frame.at;
    }
    m_position->setText(formatPosition(at, length));
}

// While a scan shows, nothing moves the replay or focuses the camera, and Capture gives way to Review's Save
// (spec §3.1); this runs with every refresh, so nothing else may enable them meanwhile.
void LabWindow::updateReplayControls(const lab_core::EngineStatus& status) {
    const bool scan = scanShowing();
    const bool replay = m_running && !m_cameraRunning && !scan;
    const bool jumps = replay && status.canSeek && status.frameRate.has_value();
    m_back10->setEnabled(jumps);
    m_forward10->setEnabled(jumps);
    m_back10Action->setEnabled(jumps);
    m_forward10Action->setEnabled(jumps);
    updatePosition(status);
    const bool playing = status.state == lab_core::EngineState::Playing;
    m_playPause->setEnabled(replay);
    if (playing != (m_playPause->text() == tr("Pause"))) {
        m_playPause->setText(playing ? tr("Pause") : tr("Play"));
        m_playPause->setIcon(playing
                                 ? qt_common::makeOutlineIcon(qt_common::OutlineIcon::Pause)
                                 : qt_common::makeColouredIcon(qt_common::OutlineIcon::Play, qt_common::kGo));
    }
    m_playPauseAction->setEnabled(replay);
    m_step->setEnabled(replay);
    m_stepAction->setEnabled(replay);
    m_stepBack->setEnabled(replay && status.canStepBack);
    m_stepBackAction->setEnabled(replay && status.canStepBack);
    m_restart->setEnabled(replay);
    m_loop->setEnabled(!scan);
    m_asFast->setEnabled(!scan);
    m_slider->setEnabled(!scan);
    m_focusAction->setEnabled(m_cameraRunning && !scan);
    m_record->setEnabled(m_running || m_record->isChecked());
    m_capture->setEnabled(m_onScreen.has_value() && !scan);
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
    std::vector<std::string> findings;
    for (const lab_core::Finding& finding : m_onScreen->processed.findings) {
        findings.push_back(lab_core::toText(finding));
    }
    const frame_io::CaptureInfo info{.source = m_engine->status().source,
                                     .findings = std::move(findings),
                                     .overlays = m_onScreen->processed.overlays};
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
        // The lab's channels end with it, unless an older viewer would only log the ends as rejected.
        m_engine->stopSending(m_stream != Stream::Off && !olderViewer());
        m_stream = Stream::Off;
        m_link->stop();
    }
    if (m_dependencies.engineMode == lab_core::Engine::Mode::Immediate) {
        m_engine->pump();
    }
    if (m_dependencies.linkMode == lab_core::ViewerLink::Mode::Immediate) {
        m_link->pump();
    }
    if (m_engine->shutDown() && m_link->idle()) {
        m_closeTimer->stop();
        event->accept();
        return;
    }
    event->ignore();
    refreshStatus();
    m_closeTimer->start(kClosePollMs);
}

// The results' tones come in the appearance's own shades, so a change of appearance colours them again.
void LabWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        colourResults();
    }
    QMainWindow::changeEvent(event);
}

// Space plays and pauses unless a button has the focus, which Space then presses. In the address field too:
// an address never holds a space.
bool LabWindow::eventFilter(QObject* watched, QEvent* event) {
    // A4's keys while a scan shows (spec §3.4's table): in Scanned, Enter skips the chip and Esc scans again.
    // Not while a text field has the focus (its Enter finishes its editing; a key it ignores reaches its
    // parents here too), nor while a list's popup is open.
    if (event->type() == QEvent::KeyPress && scanShowing()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const auto* widget = qobject_cast<QWidget*>(watched);
        const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        const bool escape = key->key() == Qt::Key_Escape;
        const bool plain = (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if ((enter || escape) && plain && widget != nullptr && widget->window() == this &&
            qobject_cast<QLineEdit*>(focusWidget()) == nullptr &&
            QApplication::activePopupWidget() == nullptr) {
            if (const std::optional<ScanEvent> scanEvent =
                    eventFor(m_scanState, enter ? ScanKey::Enter : ScanKey::Escape)) {
                applyScanEvent(*scanEvent);
                return true;
            }
        }
    }
    if (event->type() == QEvent::KeyPress && watched == m_endpoint) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
            if (m_playPauseAction->isEnabled()) {
                togglePlay();
            }
            return true;
        }
    }
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

// --- The viewer row
// -----------------------------------------------------------------------------------------

QString LabWindow::endpointText() const { return m_endpoint->text().trimmed(); }

// Another computer gets frames that may show real passports, unencrypted: it is not even pinged until the
// user has said yes to that endpoint, once per session.
bool LabWindow::mayAsk(const QString& endpoint) const {
    return visual_debug::isLoopbackEndpoint(endpoint.toStdString()) ||
           m_confirmedEndpoints.contains(endpoint);
}

bool LabWindow::confirmFor(const QString& endpoint) {
    if (endpoint.isEmpty()) {
        return false;
    }
    if (mayAsk(endpoint)) {
        return true;
    }
    const QString question =
        tr("Frames may show real passports and travel unencrypted to %1. Send them?").arg(hostOf(endpoint));
    const bool yes = m_dependencies.confirm ? m_dependencies.confirm(question)
                                            : QMessageBox::question(this, tr("Send to Another Computer"),
                                                                    question) == QMessageBox::Yes;
    if (yes) {
        m_confirmedEndpoints.insert(endpoint);
        applyEndpoint(); // pinged from now on
    }
    return yes;
}

void LabWindow::applyEndpoint() {
    const QString endpoint = endpointText();
    m_link->setEndpoint(!endpoint.isEmpty() && mayAsk(endpoint) ? endpoint.toStdString() : std::string());
}

// An older viewer logs each end as a rejected request: ■ and closing leave its channels to go idle.
bool LabWindow::olderViewer() const {
    return m_link->state().viewer == visual_debug::ViewerState::PredatesPing;
}

// ▶: starts the stream, asking first for another computer, or goes on after ⏸. The address as typed counts:
// on macOS a click on a button leaves the field focused, so its editing never finished.
void LabWindow::startStream() {
    endpointChanged();
    if (m_stream == Stream::Paused) {
        m_engine->pauseSending(false);
        m_stream = Stream::On;
        refreshStatus();
        return;
    }
    const QString endpoint = endpointText();
    if (m_stream != Stream::Off || m_closing || !confirmFor(endpoint)) {
        refreshStatus();
        return;
    }
    const std::string utf8 = endpoint.toStdString();
    m_sendingHost = visual_debug::isLoopbackEndpoint(utf8) ? QString() : hostOf(endpoint);
    m_engine->setSender(
        std::make_unique<lab_core::ResultSender>(m_dependencies.senderMode, m_dependencies.makeSender(utf8)));
    m_stream = Stream::On;
    refreshStatus();
}

// ⏸: the viewer keeps the lab's channels, showing the last frame sent.
void LabWindow::pauseStream() {
    if (m_stream != Stream::On) {
        return;
    }
    m_engine->pauseSending(true);
    m_stream = Stream::Paused;
    refreshStatus();
}

// ■: the stream stops and the lab's channels end in the viewer; ▶ waits until they have.
void LabWindow::stopStream() {
    if (m_stream == Stream::Off) {
        return;
    }
    m_engine->stopSending(!olderViewer());
    m_stream = Stream::Off;
    m_sendingHost.clear();
    refreshStatus();
}

// The picture Show shows, into the viewer's history: the frame with its tags and the processor's overlays
// whatever the Overlays toggle (the viewer has its own), or a processor's picture with its description.
void LabWindow::sendFrame() {
    endpointChanged(); // the address as typed, as for ▶
    if (!m_onScreen || !confirmFor(endpointText())) {
        refreshStatus();
        return;
    }
    const std::string entry = currentShow();
    const std::string number = std::to_string(m_onScreen->frame.number + 1);
    if (entry == kFrameEntry.toStdString()) {
        visual_debug::ImageDescription description = m_onScreen->frame.tags;
        description.overlays.insert(description.overlays.end(), m_onScreen->processed.overlays.begin(),
                                    m_onScreen->processed.overlays.end());
        m_link->sendStill(m_onScreen->frame.image, "lab frame " + number, std::move(description));
    } else {
        const auto image = std::find_if(
            m_onScreen->processed.images.begin(), m_onScreen->processed.images.end(),
            [&entry](const lab_core::ProcessedImage& candidate) { return candidate.name == entry; });
        if (image == m_onScreen->processed.images.end()) {
            m_message = tr("%1 is not in this frame").arg(QString::fromStdString(entry));
            refreshStatus();
            return;
        }
        m_link->sendStill(image->image, "lab " + entry + " " + number, image->description);
    }
    refreshStatus();
}

// A new endpoint ends the stream in the old viewer; ▶ starts it on the new one, never by itself, so no late
// end from the old sender can race a new stream.
void LabWindow::endpointChanged() {
    const QString endpoint = endpointText();
    if (endpoint == m_settings.endpoint()) {
        return;
    }
    m_settings.setEndpoint(endpoint);
    stopStream();
    applyEndpoint();
    refreshStatus();
}

void LabWindow::updateViewerRow(const lab_core::EngineStatus& status) {
    using visual_debug::ViewerState;
    const QString endpoint = endpointText();
    const lab_core::ViewerLink::State link = m_link->state();
    enum class Mark { None, Tick, Cross, Pause };
    Mark mark = Mark::None;
    QString text;
    if (endpoint.isEmpty()) {
        text = tr("no address");
    } else if (!mayAsk(endpoint)) {
        text = tr("another computer: not checked");
    } else if (!link.viewer) {
        text = tr("checking…");
    } else {
        switch (*link.viewer) {
        case ViewerState::Listening:
            mark = Mark::Tick;
            text = tr("available");
            break;
        case ViewerState::PredatesPing: {
            // It cannot answer a ping: while streaming, the stream's own results say whether it is there.
            const std::optional<visual_debug::SendResult> last =
                m_stream != Stream::Off && status.sending ? status.sending->last : std::nullopt;
            const bool gone = last == visual_debug::SendResult::ViewerUnavailable ||
                              last == visual_debug::SendResult::Timeout;
            mark = gone ? Mark::Cross : Mark::Tick;
            text = gone ? tr("not available") : tr("available (older viewer)");
            break;
        }
        case ViewerState::Paused:
            mark = Mark::Pause;
            text = tr("paused");
            break;
        case ViewerState::Unavailable:
            mark = Mark::Cross;
            text = tr("not available");
            break;
        case ViewerState::InvalidEndpoint:
            mark = Mark::Cross;
            text = tr("invalid address");
            break;
        }
    }
    if (m_viewerState->text() != text) {
        m_viewerState->setText(text);
        const QSize size = m_viewerIndicator->size();
        switch (mark) {
        case Mark::None:
            m_viewerIndicator->clear();
            break;
        case Mark::Tick:
            m_viewerIndicator->setPixmap(
                qt_common::makeColouredIcon(qt_common::OutlineIcon::Tick, qt_common::kGo).pixmap(size));
            break;
        case Mark::Cross:
            m_viewerIndicator->setPixmap(
                qt_common::makeColouredIcon(qt_common::OutlineIcon::Cross, qt_common::kHalt).pixmap(size));
            break;
        case Mark::Pause:
            m_viewerIndicator->setPixmap(
                qt_common::makeOutlineIcon(qt_common::OutlineIcon::Pause).pixmap(size));
            break;
        }
    }
    const bool hasAddress = !endpoint.isEmpty();
    m_streamButton->setEnabled(!m_closing && hasAddress && m_stream != Stream::On && !status.endingChannels);
    m_pauseStream->setEnabled(m_stream == Stream::On);
    m_stopStream->setEnabled(m_stream != Stream::Off);
    m_sendFrame->setEnabled(!m_closing && hasAddress && m_onScreen.has_value() && !scanShowing());
}

} // namespace document_lab
