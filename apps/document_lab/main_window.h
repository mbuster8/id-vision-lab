#pragma once

#include "camera_backend.h"
#include "camera_frame.h"
#include "image_view.h"
#include "lab_settings.h"
#include "live_converter.h"
#include "scan_banner.h"
#include "scan_words.h"

#include <frame_io/recording.h>
#include <lab_core/engine.h>
#include <lab_core/processors.h>
#include <lab_core/viewer_link.h>
#include <visual_debug/image_description.h>
#include <visual_debug/image_sender.h>

#include <QMainWindow>
#include <QSet>
#include <QString>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QAction;
class QCloseEvent;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSplitter;
class QTimer;
class QTreeWidget;

namespace document_lab {

using Clock = std::chrono::steady_clock;

// "0:16 / 0:25", or "1:02:03 / 1:10:00" from an hour: a replay's position and its length, seconds truncated.
QString formatPosition(std::chrono::nanoseconds at, std::chrono::nanoseconds length);

// Everything the window would otherwise make or ask for itself, so that its tests can run it without cameras,
// threads, dialogs or a disk that fills up.
struct LabDependencies {
    std::unique_ptr<CameraBackend> cameras;
    // The stream's sender, and the viewer link's own (pings, Send Frame); null: a visual_debug::Client.
    std::function<std::unique_ptr<visual_debug::ImageSender>(const std::string& endpoint)> makeSender;
    lab_core::ViewerLink::MakeSender makeLinkSender;
    lab_core::ViewerLink::Mode linkMode = lab_core::ViewerLink::Mode::Threaded;
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
    QString modelFolder;                      // empty: defaultModelFolder(), beside the program
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
    // Immediate modes: one engine pump only; its hooks reach the window queued, as in the running lab.
    void pumpEngineForTests();
    struct CanvasCounts {
        std::uint64_t received = 0; // pictures the converter made
        std::uint64_t applied = 0;  // of those, drawn: the others were for a view no longer shown
    };
    CanvasCounts canvasCountsForTests() const;

protected:
    // Waits, with the window open and its progress shown, until the recording has written every frame.
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    // A change of appearance colours the results' values again.
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void showResultsPanel(bool shown);
    void beginRun();
    void takeResult();
    const cv::Mat* shownImage(visual_debug::ImageDescription& description) const;
    void updateShowEntries(const lab_core::LabResult& result);
    void forgetShowEntries();
    void showResults(const lab_core::LabResult& result);
    void colourResults();
    void display(ImageView::KeepView keep);
    void applyPicture(LiveResult result);
    void redrawForView();
    void viewChanged();
    void refreshStatus();
    void chooseProcessor(int index);
    bool documentChosen() const;
    // The guidance of the result the canvas shows (the newest while the placeholder shows), of this run and
    // processor; null when there is none.
    const lab_core::Guidance* drawnGuidance() const;
    // The banner, the chips and the canvas's empty text, from the drawn guidance or the window's state.
    void updateScan();
    void chooseShow(int index);
    std::string currentShow() const;
    Clock::time_point now() const;
    void updateStartStop();

    // Cameras.
    void fillCameras();
    void fillFormats();
    void chooseCamera(int index);
    void chooseFormat(int index);
    void startOrStop();
    void startCamera();
    void startCameraGranted();
    void restartCamera();
    void cameraFrame(const QVideoFrame& frame, quint64 session);
    void cameraFailed(const QString& reason);
    const CameraInfo* currentCamera() const;
    std::optional<CameraFormat> currentFormat() const;

    // Replay.
    void openFile();
    void openFolder();
    void openReplay(const QString& path);
    void togglePlay();
    void seekSeconds(int seconds); // back when negative, at the replay's own rate
    void sliderChanged(int value);
    void updateReplayControls(const lab_core::EngineStatus& status);
    void updatePosition(const lab_core::EngineStatus& status);

    // Recording and capture.
    QString labFolder() const;
    void toggleRecord(bool on);
    bool startRecording(QString& failure);
    void capture();
    void captured(const std::filesystem::path& file, const std::string& error);
    void chooseLabFolder();
    void openLabFolder();

    // The viewer row.
    QString endpointText() const;
    bool mayAsk(const QString& endpoint) const; // loopback, or another computer the user said yes to
    bool confirmFor(const QString& endpoint);   // mayAsk, after asking for another computer if need be
    void applyEndpoint();                       // the link pings the endpoint, when it may
    bool olderViewer() const;
    void startStream();
    void pauseStream();
    void stopStream();
    void sendFrame();
    void endpointChanged();
    void updateViewerRow(const lab_core::EngineStatus& status);

    LabDependencies m_dependencies;
    LabSettings m_settings;

    QComboBox* m_cameraCombo = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QPushButton* m_startStop = nullptr;
    QPushButton* m_openFile = nullptr;
    QPushButton* m_openFolder = nullptr;
    QPushButton* m_playPause = nullptr;
    QPushButton* m_step = nullptr;
    QPushButton* m_stepBack = nullptr;
    QPushButton* m_restart = nullptr;
    QPushButton* m_loop = nullptr;
    QPushButton* m_asFast = nullptr;
    QPushButton* m_back10 = nullptr;
    QPushButton* m_forward10 = nullptr;
    QSlider* m_slider = nullptr;
    QLabel* m_position = nullptr;
    QAction* m_playPauseAction = nullptr;
    QAction* m_stepAction = nullptr;
    QAction* m_stepBackAction = nullptr;
    QAction* m_back10Action = nullptr;
    QAction* m_forward10Action = nullptr;
    QPushButton* m_record = nullptr;
    QPushButton* m_capture = nullptr;
    QPushButton* m_openLabFolder = nullptr;
    QPushButton* m_chooseLabFolder = nullptr;
    QTimer* m_closeTimer = nullptr;
    QLineEdit* m_endpoint = nullptr;
    QLabel* m_viewerIndicator = nullptr;
    QLabel* m_viewerState = nullptr;
    QPushButton* m_streamButton = nullptr;
    QPushButton* m_pauseStream = nullptr;
    QPushButton* m_stopStream = nullptr;
    QPushButton* m_sendFrame = nullptr;
    enum class Stream { Off, On, Paused };
    Stream m_stream = Stream::Off;
    QString m_sendingHost;              // another computer's, while streaming there; empty for this one
    QSet<QString> m_confirmedEndpoints; // the user said yes to sending there, this session
    QComboBox* m_processorCombo = nullptr;
    QComboBox* m_showCombo = nullptr;
    QPushButton* m_overlaysCheck = nullptr;
    ImageView* m_canvas = nullptr;
    QLabel* m_placeholder = nullptr;
    ScanBanner* m_banner = nullptr;
    GateChips* m_chips = nullptr;
    QSplitter* m_splitter = nullptr;
    QTreeWidget* m_results = nullptr;
    QAction* m_resultsAction = nullptr;
    QLabel* m_status = nullptr;
    QTimer* m_statusTimer = nullptr;

    QString m_wantedShow; // the Show entry chosen last, selected again when a frame has it
    QString m_message;    // the window's own: a refused permission, a camera gone
    // Why nothing runs, for the Document processor's banner: a refused permission or a camera gone; cleared
    // by the next run. No camera, or one and nothing running, the banner reads from m_cameras.
    std::optional<WindowState> m_windowState;
    QString m_windowStateDetail; // the camera's failure

    std::vector<CameraInfo> m_cameras;   // as the camera picker lists them
    std::vector<CameraFormat> m_formats; // the chosen camera's, as the format picker lists them
    bool m_cameraRunning = false;
    quint64 m_session = 0;                           // the camera session whose frames are taken
    std::optional<Clock::time_point> m_firstFrameAt; // the run's first camera frame's arrival
    CameraGeometry m_geometry;

    bool m_running = false;                // a camera or a replay runs: its results are taken
    std::uint64_t m_minimumGeneration = 0; // the run's: results of earlier ones are dropped
    std::string m_replayKind;              // "video", "images" or "recording", for a recording made of it
    bool m_recordingAsked = false;         // Record clicked, and the engine not yet recording
    bool m_closing = false;                // closing waits for the recording; the run is already stopped
    std::optional<lab_core::LabResult> m_current;  // the result taken last
    std::optional<lab_core::LabResult> m_previous; // the one before, whose picture may still be on screen
    std::uint64_t m_previousSequence = 0;
    std::optional<lab_core::LabResult> m_onScreen; // the result whose picture the canvas shows: Capture's
    std::uint64_t m_onScreenSequence = 0;          // its number among the results taken
    std::uint64_t m_scanFrom = 0; // results numbered from this on are of the processor and run chosen now
    std::uint64_t m_resultSequence = 0; // a number per result taken, for the converter's jobs
    std::uint64_t m_viewCounter = 0;    // bumped by each change of what is shown
    bool m_fitPending = false;
    bool m_fitNext = true; // the next result is fitted: the first, and the first after a stop or a new source
    std::uint64_t m_notDrawn = 0;
    CanvasCounts m_canvasCounts;
    std::deque<Clock::time_point> m_taken; // results taken in the last second
    std::deque<Clock::time_point> m_drawn; // pictures drawn in the last second

    // Destroyed in this order, from the bottom: the link's thread, the engine's and the converter's stop
    // before anything they call back into goes.
    std::unique_ptr<LiveConverter> m_converter;
    std::unique_ptr<lab_core::Engine> m_engine;
    std::unique_ptr<lab_core::ViewerLink> m_link;
};

} // namespace document_lab
