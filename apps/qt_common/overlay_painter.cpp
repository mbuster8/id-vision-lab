#include "overlay_painter.h"

#include <QPainter>
#include <QPolygonF>
#include <QRectF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace qt_common {
namespace {

template <class... F> struct Overloaded : F... {
    using F::operator()...;
};
template <class... F> Overloaded(F...) -> Overloaded<F...>;

constexpr double kTabPadding = 3.0;    // around the label's text, horizontally
constexpr double kTabTextMargin = 1.0; // above and below it
// Beyond the window, so a shape just outside it, whose edge or tab still reaches in, is drawn.
constexpr double kWindowMargin = 16.0;

QColor colourOf(viewer_core::Colour colour, double opacity = 1.0) {
    QColor result(colour.r, colour.g, colour.b);
    result.setAlphaF(static_cast<float>(opacity));
    return result;
}

// What the painter shows, in its own coordinates.
QRectF visibleArea(const QPainter& painter) {
    const QRectF window = painter.hasClipping() ? painter.clipBoundingRect() : QRectF(painter.window());
    return window;
}

void paintTab(QPainter& painter, const OverlayPainter::Label* label, QPointF bottomLeft,
              const QRectF& visible, viewer_core::Colour colour) {
    if (label == nullptr) {
        return;
    }
    const double width = std::ceil(label->size.width() + 2 * kTabPadding);
    const double height = std::ceil(label->size.height() + 2 * kTabTextMargin);
    // Kept inside what is shown, so the label of a shape whose corner is out of view still reads.
    const double x =
        std::clamp(bottomLeft.x(), visible.left(), std::max(visible.left(), visible.right() - width));
    double y = bottomLeft.y() - height;
    if (y < visible.top()) {
        y = visible.top();
    }
    const QRectF tab(x, y, width, height);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colourOf(colour));
    painter.drawRect(tab);
    painter.setPen(labelTextColour(colour));
    painter.drawStaticText(
        QPointF(x + (width - label->size.width()) / 2, y + (height - label->size.height()) / 2), label->text);
}

// The three closed shapes, in widget coordinates, drawn with QPainter's own call for each, which is cheaper
// than the same outline as a QPainterPath.
struct Outline {
    enum class Kind { Polygon, Rectangle, Ellipse } kind;
    QPolygonF points{}; // a polygon's
    QRectF rect{};      // a rectangle's, or an ellipse's bounding box
    QRectF extent() const { return kind == Kind::Polygon ? points.boundingRect() : rect; }
};

void paintShape(QPainter& painter, const Outline& outline, const QRectF& bounds, const QRectF& visible,
                const OverlayPainter::Label* label, viewer_core::Colour colour) {
    // Qt's rasteriser clips a shape reaching far beyond the window itself (measured: no slower at ±1e6 px);
    // a shape entirely outside it is only skipped.
    const QRectF extent = outline.extent();
    // Widened by a pixel: QRectF::intersects is false for a rectangle of no width or height, which a polygon
    // whose points share one x or one y has.
    if (!extent.adjusted(-1, -1, 1, 1).intersects(bounds)) {
        return;
    }
    const auto draw = [&painter, &outline] {
        switch (outline.kind) {
        case Outline::Kind::Polygon:
            painter.drawPolygon(outline.points);
            break;
        case Outline::Kind::Rectangle:
            painter.drawRect(outline.rect);
            break;
        case Outline::Kind::Ellipse:
            painter.drawEllipse(outline.rect);
            break;
        }
    };
    QPen pen(colourOf(colour), kOverlayEdgeWidth);
    pen.setCosmetic(true);
    pen.setJoinStyle(Qt::MiterJoin);
    // The fill without antialiasing: the edge, drawn over it, covers its boundary.
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colourOf(colour, kOverlayFillOpacity));
    draw();
    painter.setRenderHint(QPainter::Antialiasing, outline.kind != Outline::Kind::Rectangle);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    draw();
    // On the edge's outer side, above the shape's top-left corner.
    const double half = kOverlayEdgeWidth / 2;
    paintTab(painter, label, {extent.left() - half, extent.top() - half}, visible, colour);
}

// A Guide polygon: the halo, then the line over it, both with round joins and no fill or tab.
void paintGuide(QPainter& painter, const QPolygonF& points, const QRectF& bounds,
                viewer_core::Colour colour) {
    if (!points.boundingRect().adjusted(-1, -1, 1, 1).intersects(bounds)) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    for (const auto& [shade, width] : {std::pair{QColor(0, 0, 0, kGuideHaloAlpha), kGuideHaloWidth},
                                       std::pair{colourOf(colour), kGuideWidth}}) {
        QPen pen(shade, width);
        pen.setCosmetic(true);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPolygon(points);
    }
}

// A convex polygon moved `distance` outwards: each edge's line pushed away from the centroid, the corners
// where neighbouring lines meet. Fewer than three points come back as they are.
QPolygonF offsetOutwards(const QPolygonF& points, double distance) {
    const qsizetype n = points.size();
    if (n < 3) {
        return points;
    }
    QPointF centroid;
    for (const QPointF& point : points) {
        centroid += point;
    }
    centroid /= static_cast<double>(n);
    // Each edge's line, moved: a point on it and its direction.
    std::vector<std::pair<QPointF, QPointF>> lines;
    lines.reserve(static_cast<std::size_t>(n));
    for (qsizetype i = 0; i < n; ++i) {
        const QPointF from = points[i];
        const QPointF direction = points[(i + 1) % n] - from;
        const double length = std::hypot(direction.x(), direction.y());
        if (length == 0) {
            lines.push_back({from, QPointF(1, 0)});
            continue;
        }
        QPointF normal(direction.y() / length, -direction.x() / length);
        const QPointF middle = from + direction / 2;
        if (QPointF::dotProduct(normal, middle - centroid) < 0) {
            normal = -normal;
        }
        lines.push_back({from + normal * distance, direction});
    }
    QPolygonF out;
    for (qsizetype i = 0; i < n; ++i) {
        const auto& [a, da] = lines[static_cast<std::size_t>((i + n - 1) % n)];
        const auto& [b, db] = lines[static_cast<std::size_t>(i)];
        const double cross = da.x() * db.y() - da.y() * db.x();
        if (std::abs(cross) < 1e-9) {
            out.append(b);
            continue;
        }
        const QPointF ab = b - a;
        const double t = (ab.x() * db.y() - ab.y() * db.x()) / cross;
        out.append(a + da * t);
    }
    return out;
}

// A GuideDashed polygon: outside the shape, dashes on a halo of the same dashes, flat caps (a square cap
// would lengthen every dash by the pen's width), no fill and no tab.
void paintDashedGuide(QPainter& painter, const QPolygonF& points, const QRectF& bounds,
                      viewer_core::Colour colour) {
    const QPolygonF outside = offsetOutwards(points, kDashedGuideOffset);
    if (!outside.boundingRect().adjusted(-1, -1, 1, 1).intersects(bounds)) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    for (const auto& [shade, width] : {std::pair{QColor(0, 0, 0, kGuideHaloAlpha), kDashedGuideHaloWidth},
                                       std::pair{colourOf(colour), kDashedGuideWidth}}) {
        QPen pen(shade, width);
        pen.setCosmetic(true);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::MiterJoin);
        // In units of the pen's width, so the halo's dashes lie under the line's.
        pen.setDashPattern({kDashedGuideDash / width, kDashedGuideGap / width});
        painter.setPen(pen);
        painter.drawPolygon(outside);
    }
}

void paintPoint(QPainter& painter, QPointF at, const QRectF& bounds, const QRectF& visible,
                const OverlayPainter::Label* label, viewer_core::Colour colour) {
    if (!bounds.contains(at)) {
        return;
    }
    QPen rim(QColor(0, 0, 0, 160), 1.0);
    rim.setCosmetic(true);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(rim);
    painter.setBrush(colourOf(colour));
    const double radius = kOverlayPointDiameter / 2;
    painter.drawEllipse(at, radius, radius);
    if (label != nullptr) {
        const double height = std::ceil(label->size.height() + 2 * kTabTextMargin);
        paintTab(painter, label, {at.x() + radius + 2, at.y() + height / 2}, visible, colour);
    }
}

constexpr std::size_t kMaxLabels = 1024; // a few frames' worth of labels that all differ

} // namespace

void paintOverlays(QPainter& painter, const std::vector<viewer_core::Overlay>& overlays,
                   const QTransform& imageToWidget) {
    OverlayPainter().paint(painter, overlays, imageToWidget);
}

const OverlayPainter::Label& OverlayPainter::labelFor(const std::string& utf8, const QFont& font) {
    if (font != m_font || m_labels.size() >= kMaxLabels) {
        m_labels.clear();
        m_font = font;
    }
    auto [it, added] = m_labels.try_emplace(utf8);
    if (added) {
        it->second.text.setText(singleLineLabel(utf8));
        it->second.text.setTextFormat(Qt::PlainText);
        it->second.text.prepare(QTransform(), font);
        it->second.size = it->second.text.size();
    }
    return it->second;
}

void OverlayPainter::paint(QPainter& painter, const std::vector<viewer_core::Overlay>& overlays,
                           const QTransform& imageToWidget) {
    if (overlays.empty()) {
        return;
    }
    const QFont font = painter.font();
    const auto labelOf = [&](const std::string& utf8) -> const Label* {
        return utf8.empty() ? nullptr : &labelFor(utf8, font);
    };
    painter.save();
    // Antialiasing is most of a shape's cost (measured: it made 255 rectangles four times slower), so it is
    // used only where it shows: on the edges of polygons, circles and points. Rectangles and tabs have no
    // slanted edge to smooth.
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRectF visible = visibleArea(painter);
    const QRectF bounds = visible.adjusted(-kWindowMargin, -kWindowMargin, kWindowMargin, kWindowMargin);
    for (const viewer_core::Overlay& overlay : overlays) {
        std::visit(Overloaded{
                       [&](const viewer_core::Polygon& polygon) {
                           QPolygonF points;
                           points.reserve(static_cast<qsizetype>(polygon.points.size()));
                           for (const cv::Point2d& point : polygon.points) {
                               points.append(imageToWidget.map(QPointF(point.x, point.y)));
                           }
                           if (polygon.style == viewer_core::Polygon::Style::Guide) {
                               paintGuide(painter, points, bounds, polygon.colour);
                               return;
                           }
                           if (polygon.style == viewer_core::Polygon::Style::GuideDashed) {
                               paintDashedGuide(painter, points, bounds, polygon.colour);
                               return;
                           }
                           paintShape(painter, {.kind = Outline::Kind::Polygon, .points = std::move(points)},
                                      bounds, visible, labelOf(polygon.label), polygon.colour);
                       },
                       [&](const viewer_core::Rectangle& rectangle) {
                           const cv::Rect2d& r = rectangle.rect;
                           paintShape(painter,
                                      {.kind = Outline::Kind::Rectangle,
                                       .rect = imageToWidget.mapRect(QRectF(r.x, r.y, r.width, r.height))},
                                      bounds, visible, labelOf(rectangle.label), rectangle.colour);
                       },
                       [&](const viewer_core::Circle& circle) {
                           const QPointF centre =
                               imageToWidget.map(QPointF(circle.centre.x, circle.centre.y));
                           const double radius = circle.radius * std::abs(imageToWidget.m11());
                           paintShape(painter,
                                      {.kind = Outline::Kind::Ellipse,
                                       .rect = QRectF(centre.x() - radius, centre.y() - radius, 2 * radius,
                                                      2 * radius)},
                                      bounds, visible, labelOf(circle.label), circle.colour);
                       },
                       [&](const viewer_core::Point& point) {
                           paintPoint(painter, imageToWidget.map(QPointF(point.at.x, point.at.y)), bounds,
                                      visible, labelOf(point.label), point.colour);
                       },
                   },
                   overlay);
    }
    painter.restore();
}

QColor labelTextColour(viewer_core::Colour colour) {
    const auto linear = [](std::uint8_t component) {
        const double c = component / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double luminance =
        0.2126 * linear(colour.r) + 0.7152 * linear(colour.g) + 0.0722 * linear(colour.b);
    return luminance > 0.179 ? QColor(20, 20, 20) : QColor(255, 255, 255);
}

QString singleLineLabel(const std::string& utf8) {
    QString text = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    for (QChar& character : text) {
        const char16_t code = character.unicode();
        if (code < 0x20 || (code >= 0x7F && code <= 0x9F) || code == 0x2028 || code == 0x2029) {
            character = QLatin1Char(' ');
        }
    }
    return text;
}

} // namespace qt_common
