#include "outline_icons.h"

#include <QGuiApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace qt_common {
namespace {

// Every shape is drawn on a 24×24 grid with a 1.5-unit stroke, round caps and round joins.
constexpr qreal kGrid = 24.0;
constexpr qreal kStroke = 1.5;
constexpr qreal kDotRadius = 0.4; // stroked, so a dot about two units across

void addPolyline(QPainterPath& path, std::initializer_list<QPointF> points, bool closed = false) {
    auto point = points.begin();
    path.moveTo(*point);
    for (++point; point != points.end(); ++point) {
        path.lineTo(*point);
    }
    if (closed) {
        path.closeSubpath();
    }
}

void addDot(QPainterPath& path, QPointF centre) { path.addEllipse(centre, kDotRadius, kDotRadius); }

// One horizontal slider: a track broken by a round knob.
void addSlider(QPainterPath& path, qreal y, qreal knobX) {
    constexpr qreal kKnobRadius = 2.0;
    addPolyline(path, {{4.0, y}, {knobX - kKnobRadius, y}});
    path.addEllipse(QPointF(knobX, y), kKnobRadius, kKnobRadius);
    addPolyline(path, {{knobX + kKnobRadius, y}, {20.0, y}});
}

QPainterPath shapePath(OutlineIcon shape) {
    QPainterPath path;
    switch (shape) {
    case OutlineIcon::Pencil:
        addPolyline(path, {{15.0, 4.5}, {19.5, 9.0}, {9.0, 19.5}, {4.5, 19.5}, {4.5, 15.0}}, true);
        addPolyline(path, {{12.75, 6.75}, {17.25, 11.25}});
        break;
    case OutlineIcon::Duplicate:
        // The back square shows only where the front one does not cover it.
        path.moveTo(8.5, 8.5);
        path.lineTo(8.5, 5.5);
        path.quadTo(8.5, 3.5, 10.5, 3.5);
        path.lineTo(18.5, 3.5);
        path.quadTo(20.5, 3.5, 20.5, 5.5);
        path.lineTo(20.5, 13.5);
        path.quadTo(20.5, 15.5, 18.5, 15.5);
        path.lineTo(15.5, 15.5);
        path.addRoundedRect(QRectF(3.5, 8.5, 12.0, 12.0), 2.0, 2.0);
        break;
    case OutlineIcon::Trash:
        addPolyline(path, {{4.5, 6.5}, {19.5, 6.5}});
        addPolyline(path, {{9.5, 6.5}, {9.5, 4.0}, {14.5, 4.0}, {14.5, 6.5}});
        addPolyline(path, {{6.25, 6.5}, {7.25, 20.0}, {16.75, 20.0}, {17.75, 6.5}});
        addPolyline(path, {{10.25, 10.0}, {10.25, 16.5}});
        addPolyline(path, {{13.75, 10.0}, {13.75, 16.5}});
        break;
    case OutlineIcon::Pause:
        addPolyline(path, {{9.0, 5.5}, {9.0, 18.5}});
        addPolyline(path, {{15.0, 5.5}, {15.0, 18.5}});
        break;
    case OutlineIcon::Play:
        addPolyline(path, {{7.5, 5.0}, {18.5, 12.0}, {7.5, 19.0}}, true);
        break;
    case OutlineIcon::Stop:
        path.addRoundedRect(QRectF(6.0, 6.0, 12.0, 12.0), 1.5, 1.5);
        break;
    case OutlineIcon::ChevronUp:
        addPolyline(path, {{5.5, 15.0}, {12.0, 8.5}, {18.5, 15.0}});
        break;
    case OutlineIcon::ChevronDown:
        addPolyline(path, {{5.5, 9.0}, {12.0, 15.5}, {18.5, 9.0}});
        break;
    case OutlineIcon::Copy:
        // A clipboard: the board, and the clip across its top edge.
        path.addRoundedRect(QRectF(5.5, 5.0, 13.0, 15.5), 1.5, 1.5);
        path.addRoundedRect(QRectF(9.0, 3.5, 6.0, 3.0), 1.0, 1.0);
        break;
    case OutlineIcon::Clear:
        addPolyline(path, {{6.5, 6.5}, {17.5, 17.5}});
        addPolyline(path, {{17.5, 6.5}, {6.5, 17.5}});
        break;
    case OutlineIcon::Search:
        path.addEllipse(QPointF(10.5, 10.5), 6.0, 6.0);
        addPolyline(path, {{15.0, 15.0}, {20.0, 20.0}});
        break;
    case OutlineIcon::Options:
        addSlider(path, 6.0, 15.0);
        addSlider(path, 12.0, 8.5);
        addSlider(path, 18.0, 13.0);
        break;
    case OutlineIcon::Info:
        path.addEllipse(QPointF(12.0, 12.0), 9.0, 9.0);
        addDot(path, {12.0, 8.0});
        addPolyline(path, {{12.0, 11.0}, {12.0, 16.5}});
        break;
    case OutlineIcon::Warning:
        addPolyline(path, {{12.0, 3.75}, {20.75, 19.5}, {3.25, 19.5}}, true);
        addPolyline(path, {{12.0, 9.5}, {12.0, 13.5}});
        addDot(path, {12.0, 16.5});
        break;
    case OutlineIcon::Error:
        // An octagon, like a stop sign, so it differs from Info's circle at a glance.
        addPolyline(path,
                    {{8.6, 3.75},
                     {15.4, 3.75},
                     {20.25, 8.6},
                     {20.25, 15.4},
                     {15.4, 20.25},
                     {8.6, 20.25},
                     {3.75, 15.4},
                     {3.75, 8.6}},
                    true);
        addPolyline(path, {{12.0, 7.5}, {12.0, 13.0}});
        addDot(path, {12.0, 16.5});
        break;
    case OutlineIcon::Folder:
        // A tab on the back, and the open front.
        addPolyline(path, {{3.5, 18.0}, {3.5, 6.0}, {9.0, 6.0}, {11.0, 8.0}, {18.5, 8.0}, {18.5, 10.5}});
        addPolyline(path, {{3.5, 18.0}, {6.5, 10.5}, {21.0, 10.5}, {18.0, 18.0}}, true);
        break;
    case OutlineIcon::Send:
        // A paper plane, flying right.
        addPolyline(path, {{4.0, 4.5}, {20.5, 12.0}, {4.0, 19.5}, {6.75, 12.0}}, true);
        addPolyline(path, {{6.75, 12.0}, {12.5, 12.0}});
        break;
    case OutlineIcon::Camera:
        // A body with the viewfinder's hump on top, and the lens.
        addPolyline(path, {{8.5, 7.5}, {10.0, 5.0}, {14.0, 5.0}, {15.5, 7.5}});
        path.addRoundedRect(QRectF(3.0, 7.5, 18.0, 12.0), 2.0, 2.0);
        path.addEllipse(QPointF(12.0, 13.5), 3.5, 3.5);
        break;
    case OutlineIcon::EyeOff:
        // An eye, its two lids and the pupil, struck through: not watched.
        path.moveTo(2.5, 12.0);
        path.quadTo(12.0, 3.0, 21.5, 12.0);
        path.quadTo(12.0, 21.0, 2.5, 12.0);
        path.addEllipse(QPointF(12.0, 12.0), 3.0, 3.0);
        addPolyline(path, {{4.5, 4.5}, {19.5, 19.5}});
        break;
    case OutlineIcon::StepBack:
        // A bar, and a triangle pointing at it: one frame back.
        addPolyline(path, {{6.5, 6.0}, {6.5, 18.0}});
        addPolyline(path, {{18.0, 6.0}, {9.5, 12.0}, {18.0, 18.0}}, true);
        break;
    case OutlineIcon::StepForward:
        addPolyline(path, {{6.0, 6.0}, {14.5, 12.0}, {6.0, 18.0}}, true);
        addPolyline(path, {{17.5, 6.0}, {17.5, 18.0}});
        break;
    case OutlineIcon::Back10:
        // Two triangles pointing back: a jump, not a step.
        addPolyline(path, {{12.0, 6.5}, {4.0, 12.0}, {12.0, 17.5}}, true);
        addPolyline(path, {{20.0, 6.5}, {12.0, 12.0}, {20.0, 17.5}}, true);
        break;
    case OutlineIcon::Forward10:
        addPolyline(path, {{4.0, 6.5}, {12.0, 12.0}, {4.0, 17.5}}, true);
        addPolyline(path, {{12.0, 6.5}, {20.0, 12.0}, {12.0, 17.5}}, true);
        break;
    case OutlineIcon::Restart: {
        // A circle almost closed, the arrow at its end turning back to the start.
        const QRectF circle(5.0, 5.0, 14.0, 14.0);
        path.arcMoveTo(circle, 90.0);
        path.arcTo(circle, 90.0, 270.0);
        addPolyline(path, {{16.5, 14.0}, {19.0, 11.5}, {21.5, 14.0}});
        break;
    }
    case OutlineIcon::Loop:
        // A rounded loop, its arrow going round.
        path.addRoundedRect(QRectF(3.5, 7.5, 17.0, 10.0), 4.0, 4.0);
        addPolyline(path, {{12.0, 5.0}, {14.5, 7.5}, {12.0, 10.0}});
        break;
    case OutlineIcon::Fast:
        // A lightning bolt: as fast as possible.
        addPolyline(path, {{13.5, 3.0}, {5.5, 13.5}, {11.5, 13.5}, {10.5, 21.0}, {18.5, 10.5}, {12.5, 10.5}},
                    true);
        break;
    case OutlineIcon::Record:
        path.addEllipse(QPointF(12.0, 12.0), 6.0, 6.0);
        break;
    case OutlineIcon::Overlays:
        // A document's outline, seen at an angle, with its corners marked: what the processor found.
        addPolyline(path, {{5.0, 7.0}, {18.0, 5.0}, {19.0, 17.5}, {6.0, 19.0}}, true);
        addDot(path, {5.0, 7.0});
        addDot(path, {18.0, 5.0});
        addDot(path, {19.0, 17.5});
        addDot(path, {6.0, 19.0});
        break;
    case OutlineIcon::Tick:
        addPolyline(path, {{5.0, 12.5}, {10.0, 17.5}, {19.0, 7.0}});
        break;
    case OutlineIcon::Cross:
        addPolyline(path, {{6.5, 6.5}, {17.5, 17.5}});
        addPolyline(path, {{17.5, 6.5}, {6.5, 17.5}});
        break;
    }
    return path;
}

bool isFilled(OutlineIcon shape) {
    return shape == OutlineIcon::Play || shape == OutlineIcon::Stop || shape == OutlineIcon::Record;
}

// The palette's colour at paint time, or the icon's own colour when it has one; disabled is always grey.
QColor penColour(QIcon::Mode mode, QIcon::State state, const std::optional<QColor>& own) {
    const QPalette palette = QGuiApplication::palette();
    switch (mode) {
    case QIcon::Disabled:
        return palette.color(QPalette::PlaceholderText);
    case QIcon::Selected:
        return own ? *own : palette.color(QPalette::HighlightedText);
    case QIcon::Normal:
    case QIcon::Active:
        break;
    }
    if (own) {
        return *own;
    }
    return palette.color(state == QIcon::On ? QPalette::Highlight : QPalette::WindowText);
}

class OutlineIconEngine : public QIconEngine {
public:
    OutlineIconEngine(OutlineIcon shape, std::optional<QColor> colour) : m_shape(shape), m_colour(colour) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal side = std::min(rect.width(), rect.height());
        if (side <= 0) {
            return;
        }
        const QColor colour = penColour(mode, state, m_colour);
        const QPainterPath path = shapePath(m_shape);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(rect.x() + (rect.width() - side) / 2.0, rect.y() + (rect.height() - side) / 2.0);
        painter->scale(side / kGrid, side / kGrid);
        if (m_colour && isFilled(m_shape)) {
            painter->fillPath(path, colour);
        }
        painter->strokePath(path, QPen(colour, kStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QIconEngine* clone() const override { return new OutlineIconEngine(m_shape, m_colour); }

private:
    OutlineIcon m_shape;
    std::optional<QColor> m_colour;
};

} // namespace

QIcon makeOutlineIcon(OutlineIcon shape) { return QIcon(new OutlineIconEngine(shape, std::nullopt)); }

QIcon makeColouredIcon(OutlineIcon shape, QColor colour) {
    return QIcon(new OutlineIconEngine(shape, colour));
}

} // namespace qt_common
