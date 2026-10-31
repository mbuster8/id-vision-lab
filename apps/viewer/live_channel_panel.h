#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QImage;
class QMenu;
class QScrollArea;
class QVBoxLayout;
class LiveChannelRow;

// What a row says about its channel. Idle: no frame for viewer_core::kIdleAfter. Frozen: the view keeps one
// frame while the others are counted. Muted: frames are counted but never drawn.
struct LiveRowState {
    bool idle = false;
    bool frozen = false;
    bool muted = false;
};

// "live", "idle", "frozen", "muted", with " · idle" after the last two when no frames arrive.
QString liveStateText(LiveRowState state);

// The Live group at the top of the sidebar: a row per live channel, in the order they started, each with the
// newest frame, its frame rate and its state. Hidden while there is no channel. The panel only shows: the
// window decides what a click or a close does.
class LiveChannelPanel : public QWidget {
    Q_OBJECT

public:
    static constexpr int kRowHeight = 40;  // compact; independent of the thumbnails' width-scaled cells
    static constexpr int kVisibleRows = 3; // more scroll

    explicit LiveChannelPanel(QWidget* parent = nullptr);

    void addChannel(const QString& name); // appended; shows the panel. Nothing if the name is already here
    // Called on the window's tick, not per frame. A frozen row's dot still shows whether frames arrive, which
    // the rate tells. A muted row keeps the picture it had, greyed, and shows no rate.
    void updateChannel(const QString& name, const QImage& thumbnail, int framesPerSecond, LiveRowState state);
    void removeChannel(const QString& name);              // hides the panel when the last goes
    void setSelected(const std::optional<QString>& name); // quiet: emits nothing; an unknown name clears
    std::optional<QString> selectedChannel() const;
    QStringList channelNames() const;
    // The row's right-click menu: Mute or Unmute, and Close while the channel is idle. The caller owns it.
    std::unique_ptr<QMenu> createContextMenu(const QString& name);

signals:
    void channelSelected(QString name); // on every click on a row, even the selected one
    void closeRequested(QString name);
    // From the row's mute button or menu. The row shows the change once the window updates it.
    void muteRequested(QString name, bool muted);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    LiveChannelRow* rowOf(const QString& name) const;
    void fitHeight(); // as many rows as there are, up to kVisibleRows

    QScrollArea* m_scroll;
    QVBoxLayout* m_rowsLayout;
    std::vector<LiveChannelRow*> m_rows; // in the order they started
    std::optional<QString> m_selected;
};
