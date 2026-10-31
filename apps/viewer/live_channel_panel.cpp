#include "live_channel_panel.h"

#include "display_text.h"
#include "eliding_label.h"
#include "outline_icons.h"
#include "shades.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QScrollArea>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kMargin = 6; // the sidebar controls' margin
constexpr int kSpacing = 4;
constexpr int kPadding = 4;
constexpr int kThumbnailHeight = LiveChannelPanel::kRowHeight - 2 * kPadding;
constexpr int kThumbnailWidth = kThumbnailHeight * 3 / 2; // room for 4:3 and 16:9 pictures alike
constexpr int kDotSize = 8;
constexpr int kCloseSize = 20;
constexpr int kCloseIconSize = 14;

// A filled circle in the accent colour while frames arrive and grey otherwise, read at paint time so a change
// of appearance is followed. Never colour alone: the state's word stands beside it.
class StatusDot : public QWidget {
public:
    explicit StatusDot(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("liveDot"));
        setFixedSize(kDotSize, kDotSize);
    }

    void setActive(bool active) {
        if (active != m_active) {
            m_active = active;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const QPalette palette = QGuiApplication::palette();
        painter.setBrush(m_active ? qt_common::accentColour(palette)
                                  : palette.color(QPalette::PlaceholderText));
        painter.drawEllipse(rect());
    }

private:
    bool m_active = true;
};

} // namespace

// Muting unfreezes a channel, so muted and frozen never meet.
QString liveStateText(LiveRowState state) {
    if (!state.muted && !state.frozen) {
        return state.idle ? LiveChannelPanel::tr("idle") : LiveChannelPanel::tr("live");
    }
    const QString word = state.muted ? LiveChannelPanel::tr("muted") : LiveChannelPanel::tr("frozen");
    return state.idle ? LiveChannelPanel::tr("%1 · idle").arg(word) : word;
}

// One channel: its newest frame, its name, and below the name its rate and state. Painted like a thumbnail
// cell: the selection and hover greys, and a divider line underneath.
class LiveChannelRow : public QWidget {
public:
    explicit LiveChannelRow(const QString& name)
        : m_name(name), m_thumbnail(new QLabel), m_rate(new QLabel), m_dot(new StatusDot),
          m_state(new QLabel), m_mute(new QToolButton), m_close(new QToolButton) {
        setObjectName(QStringLiteral("liveRow_") + name);
        setAccessibleName(name);
        setFixedHeight(LiveChannelPanel::kRowHeight);
        setAttribute(Qt::WA_Hover); // repainted as the pointer comes and goes

        m_thumbnail->setObjectName(QStringLiteral("liveThumbnail"));
        m_thumbnail->setFixedSize(kThumbnailWidth, kThumbnailHeight);
        m_thumbnail->setAlignment(Qt::AlignCenter);
        auto* title = new ElidingLabel(Qt::ElideRight);
        title->setObjectName(QStringLiteral("liveName"));
        title->setText(singleLine(name));
        m_rate->setObjectName(QStringLiteral("liveRate"));
        m_state->setObjectName(QStringLiteral("liveState"));
        m_state->setText(liveStateText({}));

        m_close->setObjectName(QStringLiteral("liveClose"));
        m_close->setIcon(qt_common::makeOutlineIcon(qt_common::OutlineIcon::Clear));
        m_close->setIconSize(QSize(kCloseIconSize, kCloseIconSize));
        m_close->setFixedSize(kCloseSize, kCloseSize);
        m_close->setAutoRaise(true);
        m_close->setToolTip(LiveChannelPanel::tr("Close this channel"));
        m_close->setAccessibleName(LiveChannelPanel::tr("Close %1").arg(name));
        m_close->hide();

        m_mute->setObjectName(QStringLiteral("liveMute"));
        m_mute->setIcon(qt_common::makeOutlineIcon(qt_common::OutlineIcon::EyeOff));
        m_mute->setIconSize(QSize(kCloseIconSize, kCloseIconSize));
        m_mute->setFixedSize(kCloseSize, kCloseSize);
        m_mute->setAutoRaise(true);
        m_mute->setCheckable(true);
        m_mute->setAccessibleName(LiveChannelPanel::tr("Mute %1").arg(name));
        // Shown on hover like the thumbnails' icons; its place stays, so the name does not jump as it comes.
        QSizePolicy keepsItsPlace = m_mute->sizePolicy();
        keepsItsPlace.setRetainSizeWhenHidden(true);
        m_mute->setSizePolicy(keepsItsPlace);
        showMuteButton();

        auto* status = new QHBoxLayout;
        status->setContentsMargins(0, 0, 0, 0);
        status->setSpacing(kSpacing);
        status->addWidget(m_rate);
        status->addWidget(m_dot, 0, Qt::AlignVCenter);
        status->addWidget(m_state);
        status->addStretch(1);
        auto* text = new QVBoxLayout;
        text->setContentsMargins(0, 0, 0, 0);
        text->setSpacing(0);
        text->addWidget(title);
        text->addLayout(status);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(kMargin, kPadding, kMargin, kPadding);
        layout->setSpacing(kSpacing);
        layout->addWidget(m_thumbnail);
        layout->addLayout(text, 1);
        layout->addWidget(m_mute, 0, Qt::AlignVCenter);
        layout->addWidget(m_close, 0, Qt::AlignVCenter);
    }

    const QString& name() const { return m_name; }
    const LiveRowState& state() const { return m_rowState; }
    QToolButton* muteButton() const { return m_mute; }
    QToolButton* closeButton() const { return m_close; }

    void showState(const QImage& thumbnail, int framesPerSecond, LiveRowState state) {
        m_rowState = state;
        if (state.muted) {
            // The picture it had stays, greyed by the disabled label.
        } else if (thumbnail.isNull()) {
            m_thumbnail->clear();
        } else {
            const qreal ratio = m_thumbnail->devicePixelRatioF();
            QPixmap pixmap = QPixmap::fromImage(
                thumbnail.scaled(m_thumbnail->size() * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            pixmap.setDevicePixelRatio(ratio);
            m_thumbnail->setPixmap(pixmap);
        }
        m_thumbnail->setEnabled(!state.muted);
        m_mute->setChecked(state.muted);
        m_mute->setToolTip(state.muted ? LiveChannelPanel::tr("Unmute this channel")
                                       : LiveChannelPanel::tr("Mute this channel"));
        showMuteButton();
        m_rate->setText(state.muted ? QString() : LiveChannelPanel::tr("%1 fps").arg(framesPerSecond));
        m_state->setText(liveStateText(state));
        // While frames arrive. A frozen row's word says "idle" once they stop; until then the dot tells
        // whether the frames it does not show still come.
        m_dot->setActive(!state.muted && !state.idle && (!state.frozen || framesPerSecond > 0));
        m_close->setVisible(state.idle); // a channel that is still sending would start again at once
    }

    void setSelected(bool selected) {
        if (selected != m_selected) {
            m_selected = selected;
            showMuteButton();
            update();
        }
    }

    void setHovered(bool hovered) {
        m_hovered = hovered;
        showMuteButton();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        // Rows never take focus, so the selection is the unfocused grey.
        const QPalette palette = QGuiApplication::palette();
        if (m_selected) {
            painter.fillRect(rect(), qt_common::selectionGrey(palette, false));
        } else if (underMouse()) {
            painter.fillRect(rect(), qt_common::hoverGrey(palette));
        }
        painter.fillRect(0, height() - 1, width(), 1, qt_common::dividerGrey(palette));
    }

private:
    // On the row under the pointer and the row in view, as the thumbnails show their icons; always on a muted
    // row, so its state stays in sight.
    void showMuteButton() { m_mute->setVisible(m_hovered || m_selected || m_rowState.muted); }

    QString m_name;
    QLabel* m_thumbnail;
    QLabel* m_rate;
    StatusDot* m_dot;
    QLabel* m_state;
    QToolButton* m_mute;
    QToolButton* m_close;
    LiveRowState m_rowState;
    bool m_selected = false;
    bool m_hovered = false;
};

LiveChannelPanel::LiveChannelPanel(QWidget* parent)
    : QWidget(parent), m_scroll(new QScrollArea), m_rowsLayout(new QVBoxLayout) {
    auto* heading = new QLabel(tr("Live"));
    heading->setObjectName(QStringLiteral("liveHeading"));
    heading->setContentsMargins(kMargin, kMargin, kMargin, kPadding);
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);

    auto* rows = new QWidget;
    rows->setLayout(m_rowsLayout);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);
    m_rowsLayout->addStretch(1); // rows go above it
    m_scroll->setWidget(rows);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // A scroll area takes the focus on a click by default; a click on a row would take the arrow keys from
    // the thumbnails.
    m_scroll->setFocusPolicy(Qt::NoFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(m_scroll);
    hide();
}

void LiveChannelPanel::addChannel(const QString& name) {
    if (rowOf(name) != nullptr) {
        return;
    }
    auto* row = new LiveChannelRow(name);
    m_rowsLayout->insertWidget(static_cast<int>(m_rows.size()), row);
    row->installEventFilter(this);
    connect(row->closeButton(), &QToolButton::clicked, this, [this, name] { emit closeRequested(name); });
    // The button only asks: it stays as the window last said until the window updates the row.
    connect(row->muteButton(), &QToolButton::clicked, this, [this, row, name] {
        const bool muted = row->state().muted;
        row->muteButton()->setChecked(muted);
        emit muteRequested(name, !muted);
    });
    m_rows.push_back(row);
    fitHeight();
    show();
}

void LiveChannelPanel::updateChannel(const QString& name, const QImage& thumbnail, int framesPerSecond,
                                     LiveRowState state) {
    if (LiveChannelRow* row = rowOf(name)) {
        row->showState(thumbnail, framesPerSecond, state);
    }
}

void LiveChannelPanel::removeChannel(const QString& name) {
    LiveChannelRow* row = rowOf(name);
    if (row == nullptr) {
        return;
    }
    m_rows.erase(std::find(m_rows.begin(), m_rows.end(), row));
    if (m_selected == name) {
        m_selected.reset();
    }
    // Later, not now: the request may come from the row's own close button, still inside its click.
    m_rowsLayout->removeWidget(row);
    row->hide();
    row->setParent(nullptr);
    row->deleteLater();
    if (m_rows.empty()) {
        hide();
    } else {
        fitHeight();
    }
}

void LiveChannelPanel::setSelected(const std::optional<QString>& name) {
    m_selected = name && rowOf(*name) != nullptr ? name : std::nullopt;
    for (LiveChannelRow* row : m_rows) {
        row->setSelected(m_selected == row->name());
        if (m_selected == row->name()) {
            m_scroll->ensureWidgetVisible(row, 0, 0);
        }
    }
}

std::optional<QString> LiveChannelPanel::selectedChannel() const { return m_selected; }

QStringList LiveChannelPanel::channelNames() const {
    QStringList names;
    for (const LiveChannelRow* row : m_rows) {
        names << row->name();
    }
    return names;
}

QStringList LiveChannelPanel::selectableChannels() const {
    QStringList names;
    for (const LiveChannelRow* row : m_rows) {
        if (!row->state().muted) {
            names << row->name();
        }
    }
    return names;
}

void LiveChannelPanel::chooseChannel(const QString& name) {
    if (rowOf(name) != nullptr) {
        setSelected(name);
        emit channelSelected(name);
    }
}

std::unique_ptr<QMenu> LiveChannelPanel::createContextMenu(const QString& name) {
    // No parent: the caller owns the menu, and a parent would delete it a second time.
    auto menu = std::make_unique<QMenu>();
    const LiveChannelRow* row = rowOf(name);
    const bool muted = row != nullptr && row->state().muted;
    connect(menu->addAction(muted ? tr("Unmute") : tr("Mute")), &QAction::triggered, this,
            [this, name, muted] { emit muteRequested(name, !muted); });
    QAction* close = menu->addAction(tr("Close"));
    close->setEnabled(row != nullptr && row->state().idle); // as the close button
    connect(close, &QAction::triggered, this, [this, name] { emit closeRequested(name); });
    return menu;
}

// A press anywhere on a row but its buttons chooses the channel: the labels pass presses on to the row. The
// pointer coming and going shows and hides the row's mute button; a right-click opens its menu.
bool LiveChannelPanel::eventFilter(QObject* watched, QEvent* event) {
    const auto found = std::find(m_rows.begin(), m_rows.end(), watched);
    if (found == m_rows.end()) {
        return QWidget::eventFilter(watched, event);
    }
    LiveChannelRow* row = *found;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            const QString name = row->name();
            setSelected(name);
            emit channelSelected(name);
            return true;
        }
        break;
    case QEvent::Enter:
        row->setHovered(true);
        break;
    case QEvent::Leave:
        row->setHovered(false);
        break;
    case QEvent::ContextMenu:
        createContextMenu(row->name())->exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

LiveChannelRow* LiveChannelPanel::rowOf(const QString& name) const {
    const auto found = std::find_if(m_rows.begin(), m_rows.end(),
                                    [&name](const LiveChannelRow* row) { return row->name() == name; });
    return found != m_rows.end() ? *found : nullptr;
}

void LiveChannelPanel::fitHeight() {
    const auto shown = std::min<std::size_t>(m_rows.size(), kVisibleRows);
    m_scroll->setFixedHeight(static_cast<int>(shown) * kRowHeight);
}
