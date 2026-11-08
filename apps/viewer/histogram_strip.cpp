#include "histogram_strip.h"

#include "display_text.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace {

using viewer_core::DisplayRange;
using viewer_core::RangeHandle;

const QString kLogKey = QStringLiteral("viewer/histogramLog");

constexpr int kSide = 8; // room either side, so a marker at the axis's end keeps its handle
constexpr int kTop = 2;
constexpr int kReach = 4;  // how near a marker's line a press takes that end
constexpr int kHandle = 4; // half the width of a marker's handle
constexpr int kCurveAlpha = 110;

bool isFloatingPoint(int depth) { return depth == CV_32F || depth == CV_64F; }

// The colours the view shows the channels in: OpenCV's B, G, R; red and green for two channels.
QColor channelColour(int channel, int channels, const QPalette& palette) {
    if (channels == 1) {
        return palette.color(QPalette::Text);
    }
    static const QColor kRed(232, 72, 72);
    static const QColor kGreen(64, 190, 96);
    static const QColor kBlue(72, 128, 240);
    if (channels == 2) {
        return channel == 0 ? kRed : kGreen;
    }
    return channel == 0 ? kBlue : channel == 1 ? kGreen : kRed;
}

QString channelName(int channel, int channels) {
    if (channels == 1) {
        return {};
    }
    if (channels == 2) {
        return channel == 0 ? QStringLiteral("R") : QStringLiteral("G");
    }
    return channel == 0 ? QStringLiteral("B") : channel == 1 ? QStringLiteral("G") : QStringLiteral("R");
}

} // namespace

HistogramStrip::HistogramStrip(QWidget* parent)
    : QWidget(parent), m_emitTimer(new QTimer(this)), m_menu(new QMenu(this)),
      m_linearAction(m_menu->addAction(tr("Linear Counts"))),
      m_logAction(m_menu->addAction(tr("Log Counts"))), m_log(QSettings().value(kLogKey, false).toBool()) {
    setObjectName(QStringLiteral("histogramStrip"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // A drag's ranges reach the window at most once per turn of the event loop: a 4K still is converted again
    // for each, and the newest position is the only one that matters.
    m_emitTimer->setSingleShot(true);
    m_emitTimer->setInterval(0);
    connect(m_emitTimer, &QTimer::timeout, this, &HistogramStrip::emitPending);

    m_menu->setObjectName(QStringLiteral("histogramMenu"));
    m_linearAction->setObjectName(QStringLiteral("histogramLinear"));
    m_logAction->setObjectName(QStringLiteral("histogramLog"));
    auto* scales = new QActionGroup(this);
    for (QAction* action : {m_linearAction, m_logAction}) {
        action->setCheckable(true);
        scales->addAction(action);
    }
    m_logAction->setToolTip(
        tr("A peak, such as a black border or an empty mask, no longer flattens the rest"));
    (m_log ? m_logAction : m_linearAction)->setChecked(true);
    connect(m_linearAction, &QAction::triggered, this, [this] { setLogCounts(false); });
    connect(m_logAction, &QAction::triggered, this, [this] { setLogCounts(true); });

    hide(); // until the window has something to show in it
}

void HistogramStrip::setHistogram(const viewer_core::Histogram& histogram,
                                  const std::optional<viewer_core::RangeMarkers>& markers) {
    if (m_dragging) {
        m_held.emplace(histogram, markers);
        return;
    }
    m_histogram = histogram;
    m_markers = markers;
    update();
}

void HistogramStrip::clear() {
    cancelDrag();
    m_histogram = {};
    m_markers.reset();
    update();
}

void HistogramStrip::cancelDrag() {
    m_emitTimer->stop();
    m_pending.reset();
    if (!m_dragging) {
        return;
    }
    // What was applied stays drawn until a newer histogram says otherwise.
    if (m_markers) {
        m_markers->range = m_appliedRange;
    }
    endDrag();
}

bool HistogramStrip::isDragging() const { return m_dragging; }

const viewer_core::Histogram& HistogramStrip::histogram() const { return m_histogram; }

const std::optional<viewer_core::RangeMarkers>& HistogramStrip::markers() const { return m_markers; }

bool HistogramStrip::logCounts() const { return m_log; }

double HistogramStrip::xOf(double value) const {
    const QRectF plot = plotRect();
    return plot.left() + viewer_core::axisFraction(m_histogram, value) * plot.width();
}

QString HistogramStrip::binText(int x) const {
    const QRectF plot = plotRect();
    if (m_histogram.counts.empty() || x < plot.left() || x >= plot.right()) {
        return {};
    }
    const auto bins = static_cast<int>(m_histogram.counts.front().size());
    const int bin = std::clamp(static_cast<int>(std::floor(fractionAt(x) * bins)), 0, bins - 1);
    QString text;
    if (isFloatingPoint(m_histogram.depth)) {
        text = tr("%1 to %2")
                   .arg(formattedNumber(viewer_core::axisValue(m_histogram, double(bin) / bins)),
                        formattedNumber(viewer_core::axisValue(m_histogram, double(bin + 1) / bins)));
    } else {
        // Whole codes, both ends included; the last bin may run past the type's top, which no value reaches.
        const double first = m_histogram.axis.min + bin * m_histogram.binWidth;
        const double last = std::min(first + m_histogram.binWidth - 1, m_histogram.values.max);
        text = first == last ? formattedNumber(first)
                             : tr("%1 to %2").arg(formattedNumber(first), formattedNumber(last));
    }
    const auto channels = static_cast<int>(m_histogram.counts.size());
    for (int channel = 0; channel < channels; ++channel) {
        const std::vector<std::uint32_t>& counts = m_histogram.counts[static_cast<std::size_t>(channel)];
        const auto total = std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
        const double share = total > 0 ? 100.0 * counts[static_cast<std::size_t>(bin)] / double(total) : 0.0;
        const QString name = channelName(channel, channels);
        // Shares in the C locale, as the readout's numbers are.
        text += QStringLiteral(" · ") + (name.isEmpty() ? QString() : name + QLatin1Char(' ')) +
                QString::number(share, 'f', 1) + QStringLiteral(" %");
    }
    if (m_histogram.step > 1) {
        text += tr(" · sampled: 1 pixel in %1").arg(m_histogram.step * m_histogram.step);
    }
    return text;
}

QSize HistogramStrip::sizeHint() const { return {400, kHeight}; }

QSize HistogramStrip::minimumSizeHint() const { return {2 * kSide + 16, kHeight}; }

QRectF HistogramStrip::plotRect() const {
    const int textHeight = fontMetrics().height();
    return QRectF(kSide, kTop, std::max(1, width() - 2 * kSide),
                  std::max(1, height() - kTop - textHeight - 2));
}

double HistogramStrip::fractionAt(double x) const {
    const QRectF plot = plotRect();
    return (x - plot.left()) / plot.width();
}

HistogramStrip::Grip HistogramStrip::gripAt(double x) const {
    if (!m_markers) {
        return Grip::None;
    }
    const double black = xOf(m_markers->range.min);
    const double white = xOf(m_markers->range.max);
    const bool nearBlack = std::abs(x - black) <= kReach;
    const bool nearWhite = std::abs(x - white) <= kReach;
    if (nearBlack && nearWhite) {
        // Ends drawn this close together: the first move says which one.
        return std::abs(black - white) < 2 * kReach        ? Grip::Undecided
               : std::abs(x - black) < std::abs(x - white) ? Grip::Black
                                                           : Grip::White;
    }
    if (nearBlack) {
        return Grip::Black;
    }
    if (nearWhite) {
        return Grip::White;
    }
    return x > std::min(black, white) && x < std::max(black, white) ? Grip::Both : Grip::None;
}

void HistogramStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const Grip grip = gripAt(event->position().x());
    if (grip == Grip::None) {
        return;
    }
    m_dragging = true;
    m_moved = false;
    m_grip = grip;
    m_pressX = event->position().x();
    m_pressFraction = fractionAt(m_pressX);
    m_startRange = m_markers->range;
    m_appliedRange = m_startRange;
}

void HistogramStrip::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        return;
    }
    // A click whose hand trembles is not a drag: under Auto it would fix a live stream's range at this
    // frame's.
    if (!m_moved) {
        if (std::abs(event->position().x() - m_pressX) < QApplication::startDragDistance()) {
            return;
        }
        m_moved = true;
    }
    const double fraction = fractionAt(event->position().x());
    if (m_grip == Grip::Undecided) {
        if (fraction == m_pressFraction) {
            return;
        }
        m_grip = fraction < m_pressFraction ? Grip::Black : Grip::White;
    }
    const RangeHandle handle = m_grip == Grip::Black   ? RangeHandle::Black
                               : m_grip == Grip::White ? RangeHandle::White
                                                       : RangeHandle::Both;
    // A pixel's worth of values apart at least, so the ends stay apart on screen.
    const double onePixel = 2 * ((m_histogram.axis.max / 2 - m_histogram.axis.min / 2) / plotRect().width());
    const DisplayRange range =
        viewer_core::draggedRange(m_histogram, m_startRange, handle, m_pressFraction, fraction, onePixel);
    m_markers->range = range;
    m_pending = range;
    if (!m_emitTimer->isActive()) {
        m_emitTimer->start();
    }
    update();
}

void HistogramStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    // Emitted while still dragging: the window's redraw hands back a histogram, held back until endDrag.
    m_emitTimer->stop();
    emitPending();
    endDrag();
}

void HistogramStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag(); // the double-click's press may have taken an end or the middle
    emit resetRequested();
}

// The menu takes the mouse, so a drag under way would never see its release, and would hold back every new
// histogram: it ends here, without what was pending.
void HistogramStrip::contextMenuEvent(QContextMenuEvent* event) {
    cancelDrag();
    m_menu->popup(event->globalPos());
}

void HistogramStrip::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

bool HistogramStrip::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = binText(help->pos().x());
        if (text.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        return true;
    }
    return QWidget::event(event);
}

void HistogramStrip::emitPending() {
    if (!m_pending) {
        return;
    }
    const DisplayRange range = *m_pending;
    m_pending.reset();
    if (range == m_appliedRange) {
        return; // dragged back to where it was: nothing to change, and Auto stays Auto
    }
    m_appliedRange = range;
    emit rangeDragged(range, m_markers && m_markers->hdr);
}

void HistogramStrip::endDrag() {
    m_dragging = false;
    m_grip = Grip::None;
    if (m_held) {
        auto [histogram, markers] = std::move(*m_held);
        m_held.reset();
        setHistogram(histogram, markers);
    }
    update();
}

void HistogramStrip::setLogCounts(bool log) {
    m_log = log;
    QSettings().setValue(kLogKey, log);
    update();
}

void HistogramStrip::paintEvent(QPaintEvent* event) {
    (void)event;
    QPainter painter(this);
    const QRectF plot = plotRect();
    const QPalette& colours = palette();
    if (m_histogram.samples == 0 && !m_markers) {
        return; // cleared: nothing in view
    }

    // The span the range maps to black and white, tinted.
    if (m_markers) {
        const double black = std::clamp(xOf(m_markers->range.min), plot.left(), plot.right());
        const double white = std::clamp(xOf(m_markers->range.max), plot.left(), plot.right());
        QColor tint = colours.color(QPalette::Highlight);
        tint.setAlpha(40);
        painter.fillRect(QRectF(QPointF(std::min(black, white), plot.top()),
                                QPointF(std::max(black, white), plot.bottom())),
                         tint);
    }

    // One curve per channel. Bins narrower than a pixel share a column, which shows the largest; heights are
    // scaled to the tallest bin of any channel.
    if (!m_histogram.counts.empty()) {
        const auto bins = static_cast<int>(m_histogram.counts.front().size());
        const int columns = std::max(1, static_cast<int>(plot.width()));
        std::uint32_t tallest = 0;
        for (const auto& counts : m_histogram.counts) {
            tallest = std::max(tallest, *std::max_element(counts.begin(), counts.end()));
        }
        const auto height = [this, tallest](std::uint32_t count) {
            if (tallest == 0) {
                return 0.0;
            }
            return m_log ? std::log1p(double(count)) / std::log1p(double(tallest)) : double(count) / tallest;
        };
        // Steps on whole pixels, so no antialiasing: on a Retina screen an antialiased path of a 1600-pixel
        // strip took several milliseconds a frame. A step is drawn only where the height changes.
        const auto channels = static_cast<int>(m_histogram.counts.size());
        QPolygonF outline;
        outline.reserve(2 * columns + 2);
        for (int channel = 0; channel < channels; ++channel) {
            const auto& counts = m_histogram.counts[static_cast<std::size_t>(channel)];
            outline.clear();
            double previous = plot.bottom();
            outline << QPointF(plot.left(), previous);
            for (int column = 0; column < columns; ++column) {
                const int first = column * bins / columns;
                const int last = std::max(first + 1, (column + 1) * bins / columns);
                std::uint32_t count = 0;
                for (int bin = first; bin < last && bin < bins; ++bin) {
                    count = std::max(count, counts[static_cast<std::size_t>(bin)]);
                }
                const double top = std::round(plot.bottom() - height(count) * plot.height());
                if (top != previous) {
                    outline << QPointF(plot.left() + column, previous) << QPointF(plot.left() + column, top);
                    previous = top;
                }
            }
            outline << QPointF(plot.right(), previous) << QPointF(plot.right(), plot.bottom());
            QColor fill = channelColour(channel, channels, colours);
            const QColor line = fill;
            fill.setAlpha(kCurveAlpha);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawPolygon(outline);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(line, 0)); // cosmetic: one device pixel whatever the screen's scale
            painter.drawPolyline(outline);
        }
    }

    const QFontMetrics metrics = fontMetrics();
    const double textTop = plot.bottom() + 1;
    QPen textPen(colours.color(QPalette::Text));
    painter.setPen(textPen);
    if (m_histogram.counts.empty()) {
        painter.drawText(plot, Qt::AlignCenter, tr("No finite values"));
    }
    if (m_histogram.nonFinite > 0) {
        painter.drawText(plot.adjusted(2, 0, -2, 0), Qt::AlignTop | Qt::AlignRight,
                         tr("%1 NaN/∞ left out").arg(m_histogram.nonFinite));
    }

    std::vector<QRectF> labels; // the row under the plot: what is written there already
    if (m_markers) {
        // HDR's top of the scale: fixed for integers, so drawn thinner and dashed.
        if (m_markers->scaleTop) {
            const double top = xOf(*m_markers->scaleTop);
            if (top >= plot.left() && top <= plot.right()) {
                painter.setPen(QPen(colours.color(QPalette::Text), 1, Qt::DashLine));
                painter.drawLine(QPointF(top, plot.top()), QPointF(top, plot.bottom()));
            }
        }
        // The ends, with a handle at the foot of each and its value below.
        const QColor markerColour = colours.color(QPalette::Text);
        for (const bool white : {false, true}) {
            const double value = white ? m_markers->range.max : m_markers->range.min;
            const double x = std::clamp(xOf(value), plot.left(), plot.right());
            painter.setPen(QPen(markerColour, 1));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            const QPointF handle[] = {{x - kHandle, plot.bottom()},
                                      {x + kHandle, plot.bottom()},
                                      {x, plot.bottom() - kHandle * 1.5}};
            painter.setBrush(markerColour);
            painter.drawPolygon(handle, 3);
            painter.setBrush(Qt::NoBrush);
            const QString label = formattedNumber(value);
            const double labelWidth = metrics.horizontalAdvance(label);
            double left = white ? x - labelWidth : x;
            left = std::clamp(left, 0.0, std::max(0.0, width() - labelWidth));
            if (white && !labels.empty() && left < labels.front().right() + 4) {
                left = std::min(labels.front().right() + 4, width() - labelWidth); // never over black's
            }
            const QRectF rect(left, textTop, labelWidth, metrics.height());
            painter.setPen(textPen);
            painter.drawText(rect, Qt::AlignLeft | Qt::AlignTop, label);
            labels.push_back(rect);
        }
    }

    // The axis's ends, what a drag can reach, in the placeholder colour so they read as the scale rather than
    // the range; each only where the markers' values leave room for it.
    if (m_histogram.counts.empty() && !m_markers) {
        return;
    }
    painter.setPen(colours.color(QPalette::PlaceholderText));
    for (const bool right : {false, true}) {
        const QString label = formattedNumber(right ? m_histogram.values.max : m_histogram.values.min);
        const double labelWidth = metrics.horizontalAdvance(label);
        const QRectF rect(right ? std::max(0.0, width() - labelWidth) : 0.0, textTop, labelWidth,
                          metrics.height());
        const bool room = std::none_of(labels.begin(), labels.end(), [&rect](const QRectF& taken) {
            return rect.adjusted(-4, 0, 4, 0).intersects(taken);
        });
        if (room) {
            painter.drawText(rect, Qt::AlignLeft | Qt::AlignTop, label);
            labels.push_back(rect);
        }
    }
}
