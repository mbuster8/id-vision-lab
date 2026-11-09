#include "main_window.h"

#include "image_tags_conversion.h"
#include "live_drawing.h"

#include <viewer_core/image_tags.h>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
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

QString qtString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
    refreshStatus();
    m_statusTimer->start(kStatusRefreshMs);
}

LabWindow::~LabWindow() {
    m_statusTimer->stop();
    m_engine.reset();    // joins: no hook comes after
    m_converter.reset(); // waits for a conversion in progress
}

void LabWindow::buildUi() {
    setWindowTitle(tr("Document Lab"));
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* sourceRow = new QHBoxLayout;
    m_startStop = new QPushButton(tr("Start"), central);
    m_startStop->setObjectName(QStringLiteral("startStopButton"));
    m_startStop->setEnabled(false);
    sourceRow->addWidget(m_startStop);
    sourceRow->addStretch();
    layout->addLayout(sourceRow);

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
    layout->addLayout(processingRow);

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

    connect(m_startStop, &QPushButton::clicked, this, &LabWindow::stop);
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
    beginRun();
    m_engine->startReplay(std::move(open));
}

// Results of an earlier run are dropped from here on: a new run's generation is at least one more.
void LabWindow::beginRun() {
    m_running = true;
    m_minimumGeneration = m_engine->status().generation + 1;
    m_fitNext = true;
    m_startStop->setText(tr("Stop"));
    m_startStop->setEnabled(true);
}

void LabWindow::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;
    m_engine->stop();
    viewChanged();
    m_current.reset();
    m_canvas->clear();
    m_placeholder->hide();
    m_results->clear();
    m_fitNext = true;
    m_startStop->setText(tr("Start"));
    m_startStop->setEnabled(false);
    refreshStatus();
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
    if (status.busyFrame && status.busySince && at - *status.busySince >= kBusyShownAfter) {
        parts << tr("processing frame %1 for %2 s")
                     .arg(*status.busyFrame + 1)
                     .arg(std::chrono::duration_cast<std::chrono::seconds>(at - *status.busySince).count());
    }
    if (!status.message.empty()) {
        parts << QString::fromStdString(status.message);
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

} // namespace document_lab
