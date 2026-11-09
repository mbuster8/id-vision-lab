#pragma once

#include "activity_log.h"
#include "data_folder.h"
#include "qt_image_sink.h"
#include "server_controls.h"
#include "storage_controller.h"

#include "image_view.h"
#include "live_converter.h"
#include "snapshot_converter.h"

#include "viewer_core/display_settings.h"
#include "viewer_core/image_history.h"
#include "viewer_core/live_channels.h"
#include "visual_debug/server.h"
#include "visual_debug/wire_format.h"

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ActivityLogPanel;
class HistogramStrip;
class ImageInfoBar;
class ImageSidebar;
class QLabel;
class QSplitter;
class QTimer;
class QToolButton;
class ThumbnailPanel;

namespace Ui {
class MainWindow;
}

// "Visual Debug Viewer — <name> — <endpoint>", or without the name when it is empty. The endpoint loses its
// "tcp://", and is always there so that several viewers can be told apart in the Dock and the Window menu.
QString viewerWindowTitle(const QString& imageName, const QString& endpoint);

// Where the live channel in view is converted: on the converter's thread, or on the UI thread before the
// frame's draw returns, for tests that must not wait.
enum class LiveConversion { Threaded, Immediate };

// The viewer: received and opened images go into one history, shown as thumbnails and in a zoomable view,
// with the original value of the pixel under the cursor in the status bar. Live channels update in place
// beside the history; the view shows either a history image or a channel, never both.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    using Clock = std::chrono::steady_clock;

    // dataRoot holds one data folder per endpoint; tests pass a temporary one.
    MainWindow(visual_debug::ServerOptions serverOptions, const QString& dataRoot, QWidget* parent = nullptr);
    // The clock times live channels: their rates, and when they go idle. Tests pass one they move, and most
    // convert live frames immediately.
    MainWindow(visual_debug::ServerOptions serverOptions, const QString& dataRoot,
               std::function<Clock::time_point()> clock, QWidget* parent = nullptr,
               LiveConversion conversion = LiveConversion::Threaded);
    ~MainWindow() override;

    // The one path for received and opened images: into the history, onto the panel, and out with whatever
    // the history evicted. The panel selects it, and so shows it, unless a rename is open or the search hides
    // it. False, with a status message, for an image the viewer cannot show.
    bool addImage(std::string name, cv::Mat image, viewer_core::ImageOrigin origin = {});

    // The image in the view, which the Edit menu and File → Save As act on.
    std::optional<viewer_core::ImageId> shownImageId() const;
    std::optional<std::string> shownLiveChannel() const; // the channel in the view instead

    // Every 500 ms while live channels exist: idle states, the rows' pictures and rates, the info strip.
    void tick(Clock::time_point now);
    QtImageSink& sinkForTests();

    // Each does nothing for an unknown id.
    void renameImage(viewer_core::ImageId id, QString name); // trimmed and cut; an empty name is ignored
    void duplicateImage(viewer_core::ImageId id);
    void deleteImage(viewer_core::ImageId id);
    void undoDelete();
    void copyImage(viewer_core::ImageId id);
    void saveImage(viewer_core::ImageId id);

    // Answers the questions before deleting stored images or the history, instead of a dialog.
    void setConfirmationForTests(std::function<bool(const QString&)> confirm);
    // Converts live frames with `conversion` from now on, in the same mode; before any frame is converted.
    void setLiveConversionForTests(LiveConverter::Conversion conversion);
    // The same for snapshots' pictures; before any snapshot is taken.
    void setSnapshotConversionForTests(SnapshotConverter::Conversion conversion);
    // Live results that reached the window, and those it showed rather than dropped as stale.
    struct LiveResultCounts {
        std::uint64_t received = 0;
        std::uint64_t applied = 0;
    };
    LiveResultCounts liveResultCountsForTests() const;
    // The UI thread's time spent on live frames so far: taking them and applying their results.
    std::chrono::nanoseconds liveUiTimeForTests() const;

signals:
    void restoreFinished(); // the stored images are back, or there were none to restore

protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // on the application

private:
    void startUp();
    void startServer();
    void togglePause();   // Pause, and Resume
    void toggleRunning(); // Stop, and Start and Try again
    void setServerState(ServerState state, const QString& text);
    QString listeningText() const;
    void logRejections();   // when the sink has new counts: now, unless a line went out in the last second
    void writeRejections(); // takes the counts and logs them, all but the paused ones while paused
    void logPausedRejections();
    void log(LogKind kind, const QString& text);
    void showLatestEntry();
    void setLogOpen(bool open);
    void onImageReceived(const visual_debug::ImageMessage& message);
    void addSnapshot(SnapshotResult result);
    // An entry just added: logged, listed, stored if kept, and whatever it evicted dropped. `cutFrom` is
    // where a snapshot was cut from its frame, for the log.
    void listAdded(const viewer_core::ImageHistory::AddResult& added, const viewer_core::ImageOrigin& origin,
                   std::optional<cv::Rect> cutFrom);
    std::optional<viewer_core::ImageId> adoptRestored(const StoredImage& image);
    void onRestoreFinished(int restored, int skipped, int dropped);
    void setKeepImages(bool keep);
    void clearHistory();
    bool confirm(const QString& question);
    void showImage(viewer_core::ImageId id);
    void selectAndShow(viewer_core::ImageId id);
    void showImageText(const viewer_core::ReceivedImage& entry);
    // The picture the view, Copy Image and Save As show: the stored display picture, or one drawn with the
    // display settings of the entry's name. Only the view's shows clipping.
    QImage shownPicture(const viewer_core::ReceivedImage& entry, bool showClipping = false) const;
    // Every picture of a history image or a live frame reaches the view here, with its tags' overlays, so
    // the view and the info strip's overlay count always describe the picture in view. A live frame's picture
    // may be of a part of it only (pictureArea, in frame pixels; empty for the whole).
    void showPicture(const QImage& picture, ImageView::KeepView keep, QSize imageSize,
                     const viewer_core::ImageTags& tags, QRect pictureArea = {});
    void setShowClipping(bool show);
    // The histogram strip for the still in view, only while Show Histogram is on; nothing in view hides it. A
    // live channel's comes with each converted frame instead.
    void updateHistogram();
    void setHistogramShown(bool shown);
    void applyDraggedRange(viewer_core::DisplayRange range, bool hdr);
    void resetRangeFromHistogram();
    void showRegionStatistics(); // in the view's region label
    void changeDisplaySettings(const viewer_core::DisplaySettings& settings);
    // The settings chosen for the name, or else the default for an image with these tags.
    viewer_core::DisplaySettings settingsFor(const std::string& name,
                                             const viewer_core::ImageTags& tags) const;
    // Sets the name's settings, or forgets them (nothing), and redraws what was drawn with the old ones.
    void setDisplaySettings(const std::string& name,
                            const std::optional<viewer_core::DisplaySettings>& settings);
    cv::Mat thumbnailPicture(viewer_core::ImageId id, QSize deviceSize) const; // for the thumbnails' maker
    void takeLiveFrames();
    void showLiveChannel(const std::string& name);
    // Every change of what the view shows goes through here first, so a live result made for what was in view
    // before is recognised as stale and dropped.
    void viewChanged();
    // The shown channel's frame to the converter: the part of it in view, at the size it will be drawn; a
    // frame whose job a newer frame's replaced before any of its pictures was drawn counts as skipped.
    void submitLiveFrame(ImageView::KeepView keep);
    // A converted frame onto the view and the histogram strip, counted as drawn once per frame; dropped
    // unless its channel is still in view and nothing else took the view meanwhile.
    void applyLiveResult(LiveResult result);
    void redrawLiveFrameForView();            // after a zoom, a fit, a resize or a pan
    void showLiveText(Clock::time_point now); // the shown channel's info strip
    void updateLiveRow(const viewer_core::LiveChannel& channel, Clock::time_point now);
    void closeLiveChannel(const std::string& name); // the close button
    // Closed or evicted. A shown channel gives the view back to the image shown before it.
    void forgetLiveChannel(const std::string& name);
    void leaveLiveChannel(); // the channel in view gives the view back to the image before it, or to nothing
    // A muted channel's frames are counted but never converted or drawn; it never takes the view.
    void setLiveMuted(const std::string& name, bool muted);
    void setLiveFrozen(bool frozen);
    void takeSnapshot();
    QImage livePicture() const; // the shown channel's frame as on screen; null without one
    void copyLiveFrame();
    void saveLiveFrame();
    void savePicture(const QImage& picture, const QString& name); // asks where, then saves
    void clearView();
    void startRename(viewer_core::ImageId id);
    void dropFromView(viewer_core::ImageId id);
    void removeEvicted(const std::vector<viewer_core::ImageId>& evicted);
    std::optional<viewer_core::ImageId> listedNeighbour(viewer_core::ImageId id) const;
    int historyIndexOf(viewer_core::ImageId id) const;
    void updateEditActions();
    void showPixel(int x, int y);
    void openFile();

    std::unique_ptr<Ui::MainWindow> m_ui;
    std::function<Clock::time_point()> m_clock; // before the sink, which shares it
    std::unique_ptr<DataFolder> m_dataFolder;   // null: everything stays in memory
    QString m_dataRoot;
    std::function<bool(const QString&)> m_confirm;
    // A copy's name obeys the wire format's limit, as received and renamed names do.
    viewer_core::ImageHistory m_history{viewer_core::kDefaultHistoryCapacity,
                                        viewer_core::kDefaultHistoryByteBudget, visual_debug::kMaxNameBytes};
    viewer_core::DisplaySettingsStore m_displaySettings; // by name, for this session
    std::unique_ptr<StorageController> m_storage; // reset first when closing, while the history still exists
    // Declared before the server, so the server's thread has stopped before the sink is destroyed.
    QtImageSink m_sink;
    visual_debug::Server m_server;
    ImageSidebar* m_sidebar = nullptr;
    ThumbnailPanel* m_thumbnails = nullptr; // the sidebar's
    ImageInfoBar* m_infoBar = nullptr;
    HistogramStrip* m_histogramStrip = nullptr; // owns a drag's state: the markers it began on included
    ImageView* m_imageView = nullptr;
    QLabel* m_pixelLabel = nullptr;
    ServerControls* m_serverControls = nullptr;
    ActivityLog* m_log = nullptr; // a child, so that tests can find it
    ActivityLogPanel* m_logPanel = nullptr;
    QSplitter* m_logSplitter = nullptr;
    QToolButton* m_logToggle = nullptr;
    QLabel* m_latestLabel = nullptr; // the newest entry while the panel is closed
    QString m_endpoint;              // as configured, for the window title
    bool m_allowRemote;
    std::optional<viewer_core::ImageId> m_currentId;
    std::optional<viewer_core::ImageId> m_lastHistoryId; // shown again when the channel in its place closes
    viewer_core::LiveChannels m_live;
    std::optional<std::string> m_liveChannel; // shown instead of m_currentId
    LiveConversion m_liveConversion;
    // Reset first when closing: their threads may still be converting.
    std::unique_ptr<LiveConverter> m_converter;
    std::unique_ptr<SnapshotConverter> m_snapshots;
    std::uint64_t m_viewCounter = 0; // bumped by viewChanged
    // A job that fits the view (KeepView::No) was submitted and no fitted picture applied yet: the view still
    // shows the zoom it will lose, so jobs made meanwhile are made for the fit, and fit too.
    bool m_fitPending = false;
    // Per channel, frames whose job a newer frame's replaced, not yet drawn: a job for one of them may still
    // be converting, so whether it was skipped is known only when a newer frame's picture is applied.
    std::map<std::string, std::set<std::uint64_t>> m_replacedFrames;
    LiveResultCounts m_liveResultCounts;
    std::chrono::nanoseconds m_liveUiTime{0};
    int m_liveUiDepth = 0;       // an immediate result is applied inside the take: counted once
    bool m_showClipping = false; // for the view only, for this session
    // The rows' pictures, by channel, with the frame count each was made at: a picture is made again only
    // for a newer frame.
    std::map<std::string, std::pair<std::uint64_t, QImage>> m_rowPictures;
    std::set<std::string> m_mutedChannels;   // by name, for this session: closed and back, still muted
    std::set<std::string> m_refusedChannels; // logged once each, until the name starts a channel
    bool m_unnamedRefusals = false;          // refused beyond those, so the next rejection line counts them
    // Each channel's arrived and lost (dropped and skipped) totals when it last started, so its idle line
    // counts that stretch.
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> m_startCounts;
    QTimer* m_liveTimer = nullptr;
    QTimer* m_rejectionTimer = nullptr; // running for a second after a rejection line
    int m_pausedRejections = 0;
};
