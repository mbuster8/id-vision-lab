#include "scan_banner.h"

#include "scan_style.h"

#include <QFontInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace document_lab {

namespace {

// The banner (spec §3.2).
constexpr int kBannerTop = 28; // from the canvas's top, as the widget's own transparent margin
constexpr int kSideClear = 16; // kept clear of each side of the canvas
constexpr int kPaddingY = 18;
constexpr int kPaddingX = 30;
constexpr double kPanelRadius = 16;
constexpr int kLineGap = 10;
constexpr int kBigSize = 32;
constexpr int kSmallestBig = 22;
constexpr int kIcon = 34;
constexpr int kIconGap = 14;
constexpr int kSmallSize = 18;

// The chips (spec §3.6).
constexpr int kChipsBottom = 22; // from the canvas's bottom, as the widget's own transparent margin
constexpr int kChipPaddingY = 10;
constexpr int kChipPaddingX = 12;
constexpr double kChipRadius = 12;
constexpr int kChipGap = 8;
constexpr int kChipSize = 14;
constexpr int kChipIcon = 14;
constexpr int kChipIconGap = 4;
constexpr int kChipRowGap = 6;

constexpr int kUnbounded = 100000;

enum class Icon { Tick, Cross, Card };

// An icon from its paths in a 24-unit box (round caps and joins), never a font glyph, so every platform draws
// the same mark.
void paintIcon(QPainter& painter, const QRectF& box, Icon icon, const QColor& colour) {
    painter.save();
    painter.translate(box.topLeft());
    painter.scale(box.width() / 24.0, box.height() / 24.0);
    QPen pen(colour, icon == Icon::Card ? 2.2 : 2.6);
    pen.setCosmetic(false);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    switch (icon) {
    case Icon::Tick: {
        QPainterPath path;
        path.moveTo(5, 12.5);
        path.lineTo(9.5, 17);
        path.lineTo(19, 7.5);
        painter.drawPath(path);
        break;
    }
    case Icon::Cross:
        painter.drawLine(QPointF(6, 6), QPointF(18, 18));
        painter.drawLine(QPointF(18, 6), QPointF(6, 18));
        break;
    case Icon::Card:
        painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        painter.drawLine(QPointF(7, 9), QPointF(11, 9));
        painter.drawLine(QPointF(7, 13), QPointF(13, 13));
        break;
    }
    painter.restore();
}

std::optional<Icon> iconOf(const BannerContent& content) {
    if (content.cardIcon) {
        return Icon::Card;
    }
    switch (content.tone) {
    case doc_vision::Tone::Good:
        return Icon::Tick;
    case doc_vision::Tone::Bad:
        return Icon::Cross;
    case doc_vision::Tone::Neutral:
        break;
    }
    return std::nullopt;
}

QFont smallLineFont(const QFont& base) {
    QFont font(base);
    font.setPixelSize(kSmallSize);
    font.setWeight(QFont::Medium);
    return font;
}

// The chips in rows no wider than `width` allows, each row's chips in gate order.
struct ChipsLayout {
    std::array<int, doc_vision::kGateCount> chipWidths{};
    std::vector<std::vector<std::size_t>> rows;
    std::vector<int> rowWidths;
    int rowHeight = 0;
    int pillWidth = 0;
    int pillHeight = 0;
};

ChipsLayout chipsLayout(const QFont& font, int width) {
    const QFontMetrics metrics(font);
    ChipsLayout layout;
    layout.rowHeight = std::max(kChipIcon, metrics.height());
    int widest = 0;
    for (std::size_t i = 0; i < doc_vision::kGateCount; ++i) {
        layout.chipWidths[i] =
            kChipIcon + kChipIconGap + metrics.horizontalAdvance(chipLabel(static_cast<doc_vision::Gate>(i)));
        widest = std::max(widest, layout.chipWidths[i]);
    }
    const int rowLimit = std::max(widest, width - 2 * kSideClear - 2 * kChipPaddingX);
    for (std::size_t i = 0; i < doc_vision::kGateCount; ++i) {
        const int chip = layout.chipWidths[i];
        if (layout.rows.empty() || layout.rowWidths.back() + kChipGap + chip > rowLimit) {
            layout.rows.emplace_back();
            layout.rowWidths.push_back(chip);
        } else {
            layout.rowWidths.back() += kChipGap + chip;
        }
        layout.rows.back().push_back(i);
    }
    const int rows = static_cast<int>(layout.rows.size());
    layout.pillWidth =
        *std::max_element(layout.rowWidths.begin(), layout.rowWidths.end()) + 2 * kChipPaddingX;
    layout.pillHeight = 2 * kChipPaddingY + rows * layout.rowHeight + (rows - 1) * kChipRowGap;
    return layout;
}

} // namespace

QFont bigLineFont(const QFont& base) {
    QFont font(base);
    font.setPixelSize(kBigSize);
    for (const QFont::Weight weight : {QFont::ExtraBold, QFont::Black}) {
        font.setWeight(weight);
        if (QFontInfo(font).weight() >= QFont::ExtraBold) {
            return font;
        }
    }
    font.setWeight(QFont::Bold);
    return font;
}

FittedLine fitBigLine(QFont font, const QString& text, int availableWidth) {
    for (int size = std::max(font.pixelSize(), kSmallestBig); size >= kSmallestBig; --size) {
        font.setPixelSize(size);
        if (QFontMetrics(font).horizontalAdvance(text) <= availableWidth) {
            return {size, text};
        }
    }
    font.setPixelSize(kSmallestBig);
    return {kSmallestBig, QFontMetrics(font).elidedText(text, Qt::ElideRight, std::max(0, availableWidth))};
}

ScanBanner::ScanBanner(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
}

void ScanBanner::setContent(std::optional<BannerContent> content) {
    const bool changed = content != m_content;
    m_content = std::move(content);
    setVisible(m_content.has_value());
    if (changed) {
        updateGeometry();
        update();
    }
}

ScanBanner::Layout ScanBanner::layoutFor(int width) const {
    Layout layout;
    if (!m_content) {
        return layout;
    }
    const int innerLimit = std::max(0, width - 2 * kSideClear - 2 * kPaddingX);
    const int iconSpace = iconOf(*m_content) ? kIcon + kIconGap : 0;
    QFont big = bigLineFont(font());
    layout.big = fitBigLine(big, m_content->bigLine, innerLimit - iconSpace);
    big.setPixelSize(layout.big.pixelSize);
    const QFontMetrics bigMetrics(big);
    layout.bigWidth = bigMetrics.horizontalAdvance(layout.big.text);
    layout.rowHeight = std::max(iconSpace > 0 ? kIcon : 0, bigMetrics.height());

    int contentWidth = iconSpace + layout.bigWidth;
    int smallHeight = 0;
    if (!m_content->smallLine.isEmpty()) {
        const QFontMetrics smallMetrics(smallLineFont(font()));
        // A pixel to spare, so a line that fits by its advance is not wrapped by rounding.
        contentWidth = std::max(
            contentWidth, std::min(smallMetrics.horizontalAdvance(m_content->smallLine) + 1, innerLimit));
        smallHeight = smallMetrics
                          .boundingRect(QRect(0, 0, std::max(1, contentWidth), kUnbounded),
                                        Qt::TextWordWrap | Qt::AlignHCenter, m_content->smallLine)
                          .height();
    }
    const int panelWidth = contentWidth + 2 * kPaddingX;
    const int panelHeight = 2 * kPaddingY + layout.rowHeight + (smallHeight > 0 ? kLineGap + smallHeight : 0);
    layout.panel = QRectF((width - panelWidth) / 2, kBannerTop, panelWidth, panelHeight);
    layout.smallText =
        QRectF(layout.panel.left() + kPaddingX, layout.panel.top() + kPaddingY + layout.rowHeight + kLineGap,
               contentWidth, smallHeight);
    return layout;
}

QSize ScanBanner::sizeHint() const {
    if (!m_content) {
        return {0, 0};
    }
    const Layout layout = layoutFor(kUnbounded);
    return {static_cast<int>(layout.panel.width()) + 2 * kSideClear,
            kBannerTop + static_cast<int>(layout.panel.height())};
}

int ScanBanner::heightForWidth(int width) const {
    return m_content ? kBannerTop + static_cast<int>(layoutFor(width).panel.height()) : 0;
}

void ScanBanner::paintEvent(QPaintEvent*) {
    if (!m_content) {
        return;
    }
    const Layout layout = layoutFor(width());
    m_fitted = layout.big;
    const ToneColours colours = bannerColours(m_content->tone);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBannerPanel);
    painter.drawRoundedRect(layout.panel, kPanelRadius, kPanelRadius);

    const std::optional<Icon> icon = iconOf(*m_content);
    const int iconSpace = icon ? kIcon + kIconGap : 0;
    double x = layout.panel.center().x() - (iconSpace + layout.bigWidth) / 2.0;
    const double rowTop = layout.panel.top() + kPaddingY;
    if (icon) {
        paintIcon(painter, QRectF(x, rowTop + (layout.rowHeight - kIcon) / 2.0, kIcon, kIcon), *icon,
                  colours.big);
        x += iconSpace;
    }
    QFont big = bigLineFont(font());
    big.setPixelSize(layout.big.pixelSize);
    painter.setFont(big);
    painter.setPen(colours.big);
    // A little wider than the text, so drawText's rectangle does not clip a glyph's overhang.
    painter.drawText(QRectF(x, rowTop, layout.bigWidth + 8, layout.rowHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, layout.big.text);
    if (!m_content->smallLine.isEmpty()) {
        painter.setFont(smallLineFont(font()));
        painter.setPen(colours.small);
        painter.drawText(layout.smallText, Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignTop,
                         m_content->smallLine);
    }
}

GateChips::GateChips(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
}

void GateChips::setStates(const doc_vision::ChipStates& states) {
    const bool changed = !(states == m_states);
    m_states = states;
    setVisible(m_states.visible);
    if (changed) {
        update();
    }
}

QFont GateChips::chipFont() const {
    QFont font(this->font());
    font.setPixelSize(kChipSize);
    font.setWeight(QFont::DemiBold);
    return font;
}

QSize GateChips::sizeHint() const {
    const ChipsLayout layout = chipsLayout(chipFont(), kUnbounded);
    return {layout.pillWidth + 2 * kSideClear, layout.pillHeight + kChipsBottom};
}

int GateChips::heightForWidth(int width) const {
    return chipsLayout(chipFont(), width).pillHeight + kChipsBottom;
}

void GateChips::paintEvent(QPaintEvent*) {
    const QFont font = chipFont();
    const ChipsLayout layout = chipsLayout(font, width());
    // Anchored to the bottom: the widget may be taller than its pill.
    const QRectF pill((width() - layout.pillWidth) / 2, height() - kChipsBottom - layout.pillHeight,
                      layout.pillWidth, layout.pillHeight);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBannerPanel);
    painter.drawRoundedRect(pill, kChipRadius, kChipRadius);
    painter.setFont(font);
    const QColor good = bannerColours(doc_vision::Tone::Good).big;
    const QColor bad = bannerColours(doc_vision::Tone::Bad).big;
    double top = pill.top() + kChipPaddingY;
    for (std::size_t row = 0; row < layout.rows.size(); ++row) {
        double x = pill.center().x() - layout.rowWidths[row] / 2.0;
        for (const std::size_t i : layout.rows[row]) {
            const bool passed = m_states.passed[i];
            const QColor colour = passed ? good : bad;
            paintIcon(painter, QRectF(x, top + (layout.rowHeight - kChipIcon) / 2.0, kChipIcon, kChipIcon),
                      passed ? Icon::Tick : Icon::Cross, colour);
            painter.setPen(colour);
            const double textLeft = x + kChipIcon + kChipIconGap;
            painter.drawText(QRectF(textLeft, top, layout.chipWidths[i] + 8, layout.rowHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, chipLabel(static_cast<doc_vision::Gate>(i)));
            x += layout.chipWidths[i] + kChipGap;
        }
        top += layout.rowHeight + kChipRowGap;
    }
}

} // namespace document_lab
