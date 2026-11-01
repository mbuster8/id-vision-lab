#include "divider_splitter.h"

#include "shades.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QRectF>
#include <QSplitterHandle>
#include <QTransform>

namespace {

constexpr qreal kGripLength = 24.0;
constexpr qreal kGripThickness = 3.0;

class DividerHandle : public QSplitterHandle {
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        paintDivider(painter, size(), orientation() == Qt::Horizontal);
    }
};

} // namespace

// Drawn as if the widgets sat side by side, with the painter turned for a divider between rows.
void paintDivider(QPainter& painter, QSize size, bool sideBySide) {
    const QPalette palette = QGuiApplication::palette();
    painter.fillRect(QRect(QPoint(0, 0), size), qt_common::panelGrey(palette));
    const int across = sideBySide ? size.width() : size.height();
    const int along = sideBySide ? size.height() : size.width();
    if (!sideBySide) {
        painter.setTransform(QTransform(0, 1, 1, 0, 0, 0)); // swaps x and y
    }
    const QColor line = qt_common::dividerGrey(palette);
    painter.fillRect(across / 2, 0, 1, along, line);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(line);
    painter.drawRoundedRect(
        QRectF(across / 2 + 0.5 - kGripThickness / 2, (along - kGripLength) / 2, kGripThickness, kGripLength),
        kGripThickness / 2, kGripThickness / 2);
}

DividerSplitter::DividerSplitter(Qt::Orientation orientation, QWidget* parent)
    : QSplitter(orientation, parent) {
    setHandleWidth(kHandleWidth);
}

QSplitterHandle* DividerSplitter::createHandle() { return new DividerHandle(orientation(), this); }
