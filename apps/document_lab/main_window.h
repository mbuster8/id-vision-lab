#pragma once

#include "camera_backend.h"
#include "image_view.h"
#include "lab_settings.h"
#include "live_converter.h"

#include <frame_io/recording.h>
#include <lab_core/engine.h>
#include <lab_core/processors.h>
#include <visual_debug/image_description.h>
#include <visual_debug/image_sender.h>

#include <QMainWindow>
#include <QString>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace document_lab {

using Clock = std::chrono::steady_clock;

// Everything the window would otherwise make or ask for itself, so that its tests can run it without cameras,
// threads, dialogs or a disk that fills up.
struct LabDependencies {
    std::unique_ptr<CameraBackend> cameras;
    std::function<std::unique_ptr<visual_debug::ImageSender>(const std::string& endpoint)> makeSender;
    std::function<std::unique_ptr<lab_core::FrameProcessor>(lab_core::ProcessorKind)>
        makeProcessor; // null: lab_core's
    lab_core::Engine::Mode engineMode = lab_core::Engine::Mode::Threaded;
    LiveConverter::Mode conversionMode = LiveConverter::Mode::Threaded;
    LiveConverter::Conversion conversion = convertLiveFrame;
    lab_core::ResultSender::Mode senderMode = lab_core::ResultSender::Mode::Threaded;
    frame_io::RecordingWriter::Mode recorderMode = frame_io::RecordingWriter::Mode::Threaded;
    std::function<void(std::uint64_t)> recorderBeforeWrite;
    bool immediateCapture = false;
    std::function<std::uintmax_t(const std::filesystem::path&)> freeSpace; // null: what the volume has
    std::function<bool(const QString& question)> confirm;                  // null: a QMessageBox
    std::function<QString()> chooseFile;
    std::function<QString()> chooseFolder;
    std::function<QString()> chooseLabFolder; // null: QFileDialog
    QString labFolder;                        // empty: the app's local data folder
    std::function<Clock::time_point()> clock; // null: steady_clock::now
};

// The Document Lab: a camera or a replay, run through a processor on the engine's thread, the result drawn on
// the viewer's canvas with its overlays, its findings listed, and its counts in the status bar.
class LabWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit LabWindow(LabDependencies dependencies, QWidget* parent = nullptr);
    ~LabWindow() override;

    // A replay of whatever `open` opens, on the engine's thread (Open File… and Open Folder… use it).
    void startReplay(lab_core::ReplayOpener open);
    // Stops the camera or the replay; the canvas and the results empty, and nothing of the run comes after.
    void stop();

    // Immediate modes: one engine pump, its result taken and drawn, the status refreshed.
    void pumpForTests();
    struct CanvasCounts {
        std::uint64_t received = 0; // pictures the converter made
        std::uint64_t applied = 0;  // of those, drawn: the others were for a view no longer shown
    };
    CanvasCounts canvasCountsForTests() const;

private:
    void buildUi();
    void beginRun();
    void takeResult();
    const cv::Mat* shownImage(visual_debug::ImageDescription& description) const;
    void updateShowEntries(const lab_core::LabResult& result);
    void showResults(const lab_core::LabResult& result);
    void display(ImageView::KeepView keep);
    void applyPicture(LiveResult result);
    void redrawForView();
    void viewChanged();
    void refreshStatus();
    void chooseProcessor(int index);
    void chooseShow(int index);
    std::string currentShow() const;
    Clock::time_point now() const;

    LabDependencies m_dependencies;
    LabSettings m_settings;

    QPushButton* m_startStop = nullptr;
    QComboBox* m_processorCombo = nullptr;
    QComboBox* m_showCombo = nullptr;
    QCheckBox* m_overlaysCheck = nullptr;
    ImageView* m_canvas = nullptr;
    QLabel* m_placeholder = nullptr;
    QListWidget* m_results = nullptr;
    QLabel* m_status = nullptr;
    QTimer* m_statusTimer = nullptr;

    QString m_wantedShow; // the Show entry chosen last, selected again when a frame has it

    bool m_running = false;                       // a camera or a replay runs: its results are taken
    std::uint64_t m_minimumGeneration = 0;        // the run's: results of earlier ones are dropped
    std::optional<lab_core::LabResult> m_current; // the result on screen
    std::uint64_t m_resultSequence = 0;           // a number per result taken, for the converter's jobs
    std::uint64_t m_viewCounter = 0;              // bumped by each change of what is shown
    bool m_fitPending = false;
    bool m_fitNext = true; // the next result is fitted: the first, and the first after a stop or a new source
    std::uint64_t m_notDrawn = 0;
    CanvasCounts m_canvasCounts;
    std::deque<Clock::time_point> m_taken; // results taken in the last second
    std::deque<Clock::time_point> m_drawn; // pictures drawn in the last second

    // Destroyed in this order, from the bottom: the converter's thread and the engine's stop before anything
    // they call back into goes.
    std::unique_ptr<LiveConverter> m_converter;
    std::unique_ptr<lab_core::Engine> m_engine;
};

} // namespace document_lab
