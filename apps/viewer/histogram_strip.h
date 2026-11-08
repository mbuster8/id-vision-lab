#pragma once

#include "viewer_core/histogram.h"

#include <QWidget>

#include <optional>
#include <utility>

class QAction;
class QMenu;
class QTimer;

// A short strip under the info strip: a histogram of the image in view, one curve per colour channel, with
// the range in use drawn on it. Dragging an end, or the middle, asks for a new range (Fixed, or HDR's black
// and white); a double-click asks for the name's default range. Starts hidden: the window shows it.
class HistogramStrip : public QWidget {
    Q_OBJECT

public:
    static constexpr int kHeight = 64;

    explicit HistogramStrip(QWidget* parent = nullptr);

    // What is in view. During a drag it is held back until the drag ends, so neither the axis nor the markers
    // the drag started on move under the cursor.
    void setHistogram(const viewer_core::Histogram& histogram,
                      const std::optional<viewer_core::RangeMarkers>& markers);
    void clear();      // nothing in view; ends a drag without emitting
    void cancelDrag(); // ends a drag without emitting what is pending, then applies a histogram held back
    bool isDragging() const;
    const viewer_core::Histogram& histogram() const; // the one drawn
    // The ones drawn: during a drag, those it started on, with the newest dragged range.
    const std::optional<viewer_core::RangeMarkers>& markers() const;
    bool logCounts() const;
    double xOf(double value) const; // within the plot, in widget coordinates
    QString binText(int x) const;   // the tooltip at x; empty off the curves
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // At most one per turn of the event loop (the newest position), and once more at the release if one is
    // pending. `hdr`: the drag began on HDR's markers, so the range is HDR's black and white, not Fixed.
    void rangeDragged(viewer_core::DisplayRange range, bool hdr);
    void resetRequested(); // a double-click

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void hideEvent(QHideEvent* event) override; // cancels a drag
    bool event(QEvent* event) override;         // tooltips: binText

private:
    // Which end a press took; Undecided when both ends were within reach, until the first move says which.
    enum class Grip { None, Black, White, Both, Undecided };

    QRectF plotRect() const;
    double fractionAt(double x) const;
    Grip gripAt(double x) const;
    void emitPending();
    void endDrag(); // applies a histogram held back during the drag
    void setLogCounts(bool log);

    viewer_core::Histogram m_histogram;
    std::optional<viewer_core::RangeMarkers> m_markers;
    // Arrived during a drag, for its end.
    std::optional<std::pair<viewer_core::Histogram, std::optional<viewer_core::RangeMarkers>>> m_held;
    bool m_dragging = false;
    bool m_moved = false; // past the platform's drag distance since the press
    Grip m_grip = Grip::None;
    double m_pressX = 0.0;
    double m_pressFraction = 0.0;
    viewer_core::DisplayRange m_startRange{};
    viewer_core::DisplayRange m_appliedRange{}; // the last range emitted, or the start's
    std::optional<viewer_core::DisplayRange> m_pending;
    QTimer* m_emitTimer;
    QMenu* m_menu;
    QAction* m_linearAction;
    QAction* m_logAction;
    bool m_log = false;
};
