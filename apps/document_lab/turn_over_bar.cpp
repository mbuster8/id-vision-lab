#include "turn_over_bar.h"

#include "shades.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>

namespace document_lab {

namespace {

constexpr int kBarPixels = 13; // as Review's bar

void setTextColour(QWidget* widget, const QColor& colour) {
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, colour);
    widget->setPalette(palette);
}

} // namespace

TurnOverBar::TurnOverBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("turnOverBar"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(14, 8, 14, 8);
    row->setSpacing(16);
    QFont font = this->font();
    font.setPixelSize(kBarPixels);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("turnOverTitle"));
    QFont bold = font;
    bold.setBold(true);
    m_title->setFont(bold);
    row->addWidget(m_title);
    row->addStretch(1);
    const auto makeButton = [&](const char* name, const QString& text) {
        auto* made = new QPushButton(text, this);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(font);
        made->setAutoDefault(false);
        made->setCursor(Qt::PointingHandCursor);
        row->addWidget(made);
        return made;
    };
    m_skipFront = makeButton("skipFrontButton", tr("Skip the front"));
    m_scanAgain = makeButton("turnOverScanAgainButton", tr("Scan again"));
    connect(m_skipFront, &QPushButton::clicked, this, &TurnOverBar::skipFront);
    connect(m_scanAgain, &QPushButton::clicked, this, &TurnOverBar::scanAgain);
    setBackOnReplay(false);
    applyColours();
}

void TurnOverBar::setBackOnReplay(bool onReplay) {
    m_title->setText(onReplay ? tr("✓ Back scanned · open the front's clip") : tr("✓ Back scanned"));
}

void TurnOverBar::applyColours() {
    m_colours = reviewColours(qt_common::isDarkAppearance(QApplication::palette()));
    setTextColour(m_title, m_colours.text);
    update();
}

void TurnOverBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), m_colours.pillBackground);
}

void TurnOverBar::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyColours();
    }
}

} // namespace document_lab
