#include "scanned_card.h"

#include "scan_style.h"

#include "shades.h"

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace document_lab {

namespace {

// Spec §3.3.
constexpr int kCardWidth = 620;
constexpr int kSideClear = 16;
constexpr int kPaddingTop = 34;
constexpr int kPaddingSide = 36;
constexpr int kPaddingBottom = 30;
constexpr int kItemGap = 18;
constexpr int kCardRadius = 20;
constexpr int kCheckSide = 72;
constexpr int kCheckMark = 40;
constexpr int kIllustrationWidth = 220;
constexpr int kIllustrationHeight = 96;
constexpr int kExplanationWidth = 470;

bool darkAppearance() { return qt_common::isDarkAppearance(QApplication::palette()); }

QFont pixelFont(const QFont& base, int pixels, QFont::Weight weight) {
    QFont font(base);
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

void setTextColour(QWidget* widget, const QColor& colour) {
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, colour);
    widget->setPalette(palette);
}

// The 72 px circle with its ✓, painted (never a font glyph).
class CheckCircle : public QWidget {
public:
    explicit CheckCircle(QWidget* parent) : QWidget(parent) { setFixedSize(kCheckSide, kCheckSide); }
    void setColours(const QColor& circle, const QColor& mark) {
        m_circle = circle;
        m_mark = mark;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_circle);
        painter.drawEllipse(rect());
        const double offset = (kCheckSide - kCheckMark) / 2.0;
        painter.translate(offset, offset);
        painter.scale(kCheckMark / 24.0, kCheckMark / 24.0);
        QPen pen(m_mark, 2.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QPainterPath tick;
        tick.moveTo(5, 12.5);
        tick.lineTo(9.5, 17);
        tick.lineTo(19, 7.5);
        painter.drawPath(tick);
    }

private:
    QColor m_circle;
    QColor m_mark;
};

// The document on the reader, as screen 04 draws it: the reader, the document sheared −12°, a green LED. An
// ID card (A4.1's D10) is drawn at the passport's scale, an ID-1 card being about 0.68 of a closed passport's
// width and 0.61 of its height, with its chip.
class ReaderIllustration : public QWidget {
public:
    explicit ReaderIllustration(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("readerIllustration"));
        setFixedSize(kIllustrationWidth, kIllustrationHeight);
    }
    void setReaderColour(const QColor& colour) {
        m_reader = colour;
        update();
    }
    void setKind(lab_core::DocumentKind kind) {
        m_kind = kind;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_reader);
        painter.drawRoundedRect(QRectF(0, 62, 220, 34), 8, 8);
        painter.save();
        painter.translate(30, 0);
        painter.setTransform(QTransform().shear(-0.2126, 0), true); // tan(12°)
        painter.setBrush(kIllustrationPassport);
        if (m_kind == lab_core::DocumentKind::Passport) {
            painter.drawRoundedRect(QRectF(20, 4, 160, 62), 4, 4);
        } else {
            painter.drawRoundedRect(QRectF(46, 28, 109, 38), 4, 4);
            painter.setBrush(m_reader);
            painter.drawRoundedRect(QRectF(58, 38, 16, 12), 2, 2); // the chip
        }
        painter.restore();
        painter.setBrush(kIllustrationLed);
        painter.drawEllipse(QPointF(196, 79), 5, 5);
    }

private:
    QColor m_reader;
    lab_core::DocumentKind m_kind = lab_core::DocumentKind::Passport;
};

// The reader line's dot.
class Dot : public QWidget {
public:
    explicit Dot(QWidget* parent) : QWidget(parent) { setFixedSize(10, 10); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kReaderDotWaiting);
        painter.drawEllipse(rect());
    }
};

} // namespace

ScannedCard::ScannedCard(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSideClear, kSideClear, kSideClear, kSideClear);
    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setAttribute(Qt::WA_TranslucentBackground);
    m_scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(m_scroll);

    auto* holder = new QWidget;
    auto* column = new QVBoxLayout(holder);
    column->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout;
    column->addStretch(1);
    column->addLayout(row);
    column->addStretch(1);
    row->addStretch(1);

    m_body = new QWidget(holder);
    m_body->setObjectName(QStringLiteral("scannedCardBody"));
    m_body->setAttribute(Qt::WA_StyledBackground);
    m_body->setMaximumWidth(kCardWidth);
    m_body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* shadow = new QGraphicsDropShadowEffect(m_body);
    shadow->setBlurRadius(60);
    shadow->setOffset(0, 24);
    shadow->setColor(QColor(0, 0, 0, 128));
    m_body->setGraphicsEffect(shadow);
    row->addWidget(m_body, 10);
    row->addStretch(1);

    auto* items = new QVBoxLayout(m_body);
    items->setContentsMargins(kPaddingSide, kPaddingTop, kPaddingSide, kPaddingBottom);
    items->setSpacing(kItemGap);
    const QFont base = font();

    m_check = new CheckCircle(m_body);
    items->addWidget(m_check, 0, Qt::AlignHCenter);

    // Centred by their own alignment, not the layout's: a word-wrapped label the layout aligns gets its size
    // hint's height, not the height its width needs, and its last lines are cut off on a narrow canvas.
    const auto label = [&](const char* name, const QString& text, int pixels, QFont::Weight weight) {
        auto* made = new QLabel(text, m_body);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(pixelFont(base, pixels, weight));
        made->setAlignment(Qt::AlignHCenter);
        made->setWordWrap(true);
        return made;
    };
    m_title = label("scannedTitle", tr("Passport scanned"), 34, QFont::ExtraBold);
    items->addWidget(m_title);
    m_headline = label("scannedHeadline", tr("Now place the passport on the chip reader"), 22, QFont::Bold);
    items->addWidget(m_headline);
    m_explanation =
        label("scannedExplanation",
              tr("Close it and lay the cover flat on the reader. For most passports the chip is in "
                 "the cover or the data page."),
              16, QFont::Normal);
    m_explanation->setMaximumWidth(kExplanationWidth);
    // Narrower than the card: centred between stretches, which keep its height for its width.
    auto* explanationRow = new QHBoxLayout;
    explanationRow->addStretch(1);
    explanationRow->addWidget(m_explanation, 100);
    explanationRow->addStretch(1);
    items->addLayout(explanationRow);

    m_illustration = new ReaderIllustration(m_body);
    items->addWidget(m_illustration, 0, Qt::AlignHCenter);

    m_pill = new QWidget(m_body);
    m_pill->setObjectName(QStringLiteral("readerPill"));
    m_pill->setAttribute(Qt::WA_StyledBackground);
    auto* pillRow = new QHBoxLayout(m_pill);
    pillRow->setContentsMargins(14, 8, 14, 8);
    pillRow->setSpacing(8);
    pillRow->addWidget(new Dot(m_pill), 0, Qt::AlignVCenter);
    // A5 will put "Waiting for the passport on <reader name>…" here, with a blue dot.
    m_readerLine = new QLabel(tr("Chip reading arrives in A5"), m_pill);
    m_readerLine->setObjectName(QStringLiteral("readerLine"));
    m_readerLine->setFont(pixelFont(base, 15, QFont::Normal));
    pillRow->addWidget(m_readerLine);
    items->addWidget(m_pill, 0, Qt::AlignHCenter);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(12);
    buttons->addStretch(1);
    const auto makeButton = [&](const char* name, const QString& text) {
        auto* made = new QPushButton(text, m_body);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(pixelFont(base, 15, QFont::DemiBold));
        made->setCursor(Qt::PointingHandCursor);
        buttons->addWidget(made);
        return made;
    };
    m_skipChip = makeButton("skipChipButton", tr("Skip the chip"));
    m_scanAgain = makeButton("scanAgainButton", tr("Scan again"));
    buttons->addStretch(1);
    items->addLayout(buttons);
    connect(m_skipChip, &QPushButton::clicked, this, &ScannedCard::skipChip);
    connect(m_scanAgain, &QPushButton::clicked, this, &ScannedCard::scanAgain);

    m_note = label(
        "scannedNote",
        tr("The captured frame is kept until you choose: save, send or discard it after the chip step."), 13,
        QFont::Normal);
    items->addWidget(m_note);

    m_scroll->setWidget(holder);
    // After setWidget, which turns it on: the holder would paint a flat fill over the dimmed frame around the
    // card.
    holder->setAutoFillBackground(false);
    applyColours();
}

void ScannedCard::setKind(lab_core::DocumentKind kind) {
    static_cast<ReaderIllustration*>(m_illustration)->setKind(kind);
    if (kind == lab_core::DocumentKind::Passport) {
        m_title->setText(tr("Passport scanned"));
        m_headline->setText(tr("Now place the passport on the chip reader"));
        m_explanation->setText(
            tr("Close it and lay the cover flat on the reader. For most passports the chip is "
               "in the cover or the data page."));
    } else {
        m_title->setText(tr("ID card scanned"));
        m_headline->setText(tr("Now place the card on the chip reader"));
        m_explanation->setText(tr("Lay the card flat on the reader."));
    }
}

void ScannedCard::applyColours() {
    const ReviewColours colours = reviewColours(darkAppearance());
    m_body->setStyleSheet(QStringLiteral("#scannedCardBody { background-color: %1; border-radius: %2px; }")
                              .arg(colours.cardBackground.name())
                              .arg(kCardRadius));
    m_pill->setStyleSheet(QStringLiteral("#readerPill { background-color: %1; border-radius: 16px; }")
                              .arg(colours.pillBackground.name()));
    static_cast<CheckCircle*>(m_check)->setColours(colours.checkCircle, colours.checkMark);
    static_cast<ReaderIllustration*>(m_illustration)->setReaderColour(colours.reader);
    setTextColour(m_title, colours.title);
    setTextColour(m_headline, colours.text);
    setTextColour(m_explanation, colours.secondary);
    setTextColour(m_readerLine, colours.text);
    setTextColour(m_note, colours.secondary);
    const QString button =
        QStringLiteral(
            "QPushButton { color: %1; background-color: %2; border: 1px solid %3; border-radius: 10px; "
            "padding: 12px 20px; }")
            .arg(colours.text.name(), colours.cardBackground.name(), colours.buttonBorder.name());
    m_skipChip->setStyleSheet(button);
    m_scanAgain->setStyleSheet(button);
}

void ScannedCard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kScannedDim);
}

void ScannedCard::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyColours();
    }
}

} // namespace document_lab
