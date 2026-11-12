#include "review_view.h"

#include "scan_words.h"

#include "shades.h"

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <string_view>

namespace document_lab {

namespace {

// Spec §3.4 and screen 06.
constexpr int kPanelWidth = 330;
constexpr int kPanelPadding = 16;
constexpr int kTitlePixels = 15;
constexpr int kRowPixels = 13;
constexpr int kButtonPixels = 14;
constexpr int kNotePixels = 12;
constexpr int kBarPixels = 13;

const QString kCheckPassed = QStringLiteral("✓");
const QString kCheckFailed = QStringLiteral("✗");
// A field the MRZ leaves empty: a '<' sex, no personal number.
const QString kEmptyField = QStringLiteral("—");

bool darkAppearance() { return qt_common::isDarkAppearance(QApplication::palette()); }

QFont pixelFont(const QFont& base, int pixels, QFont::Weight weight = QFont::Normal) {
    QFont font(base);
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

void setTextColour(QWidget* widget, const QColor& colour) {
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, colour);
    palette.setColor(QPalette::ButtonText, colour);
    palette.setColor(QPalette::Text, colour);
    widget->setPalette(palette);
}

QString fieldText(const std::string& field) {
    return field.empty() ? kEmptyField : QString::fromStdString(field);
}

std::optional<bool> passed(const doc_vision::Mrz& mrz, doc_vision::MrzCheck check) {
    const auto found =
        std::find_if(mrz.checks.begin(), mrz.checks.end(),
                     [check](const doc_vision::MrzCheckResult& result) { return result.check == check; });
    if (found == mrz.checks.end()) {
        return std::nullopt;
    }
    return found->passed;
}

QString buttonStyle(const QColor& text, const QColor& background, const QColor& border) {
    return QStringLiteral(
               "QPushButton { color: %1; background-color: %2; border: 1px solid %3; border-radius: 10px; "
               "padding: 10px 14px; }")
        .arg(text.name(), background.name(), border.name());
}

// The panel's rows, in screen 06's order; each with the check digit that covers it, if any.
struct RowSpec {
    const char* key;
    const char* label;
    std::optional<doc_vision::MrzCheck> check;
};
const RowSpec kRowSpecs[] = {
    {"surname", QT_TRANSLATE_NOOP("ReviewPanel", "Surname"), std::nullopt},
    {"givenNames", QT_TRANSLATE_NOOP("ReviewPanel", "Given names"), std::nullopt},
    {"documentNumber", QT_TRANSLATE_NOOP("ReviewPanel", "Document no."),
     doc_vision::MrzCheck::DocumentNumber},
    {"nationality", QT_TRANSLATE_NOOP("ReviewPanel", "Nationality"), std::nullopt},
    {"born", QT_TRANSLATE_NOOP("ReviewPanel", "Born"), doc_vision::MrzCheck::BirthDate},
    {"sex", QT_TRANSLATE_NOOP("ReviewPanel", "Sex"), std::nullopt},
    {"expires", QT_TRANSLATE_NOOP("ReviewPanel", "Expires"), doc_vision::MrzCheck::ExpiryDate},
    {"personalNumber", QT_TRANSLATE_NOOP("ReviewPanel", "Personal no."), doc_vision::MrzCheck::OptionalData},
    {"composite", QT_TRANSLATE_NOOP("ReviewPanel", "Composite"), doc_vision::MrzCheck::Composite},
};

} // namespace

// ---- ReviewBar ----

ReviewBar::ReviewBar(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(14, 8, 14, 8);
    row->setSpacing(16);
    const QFont base = font();
    m_title = new QLabel(tr("Captured frame"), this);
    m_title->setObjectName(QStringLiteral("reviewBarTitle"));
    m_title->setFont(pixelFont(base, kBarPixels, QFont::Bold));
    row->addWidget(m_title);
    // A4.1: a TD1 card's two sides, one on the canvas at a time.
    const auto sideButton = [&](const char* name, const QString& text, CardSide side) {
        auto* made = new QPushButton(text, this);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(pixelFont(base, kBarPixels));
        made->setCheckable(true);
        made->setAutoDefault(false);
        made->hide();
        row->addWidget(made);
        connect(made, &QPushButton::clicked, this, [this, side] { showSide(side); });
        return made;
    };
    m_backSide = sideButton("backSideButton", tr("Back"), CardSide::Back);
    m_frontSide = sideButton("frontSideButton", tr("Front"), CardSide::Front);
    m_backSide->setChecked(true);
    m_lead = new QLabel(tr("Show over it:"), this);
    m_lead->setObjectName(QStringLiteral("reviewBarLead"));
    m_lead->setFont(pixelFont(base, kBarPixels));
    row->addWidget(m_lead);
    const auto box = [&](const char* name, const QString& text, bool checked) {
        auto* made = new QCheckBox(text, this);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(pixelFont(base, kBarPixels));
        made->setChecked(checked);
        row->addWidget(made);
        connect(made, &QCheckBox::toggled, this, [this] { emit overlaysChanged(overlays()); });
        return made;
    };
    const ReviewOverlays defaults;
    m_outline = box("outlineBox", tr("Document outline"), defaults.outline);
    m_band = box("bandBox", tr("MRZ band"), defaults.band);
    m_corners = box("cornersBox", tr("Corner numbers"), defaults.corners);
    m_gates = box("gatesBox", tr("Gate values"), defaults.gates);
    m_photoZone = box("photoZoneBox", tr("Photo zone (A3.2)"), false);
    m_photoZone->setEnabled(false);
    row->addStretch(1);
    applyColours();
}

ReviewOverlays ReviewBar::overlays() const {
    return {.outline = m_outline->isChecked(),
            .band = m_band->isChecked() && m_side == CardSide::Back,
            .corners = m_corners->isChecked(),
            .gates = m_gates->isChecked()};
}

void ReviewBar::setHasFront(bool hasFront) {
    m_backSide->setVisible(hasFront);
    m_frontSide->setVisible(hasFront);
}

void ReviewBar::showSide(CardSide side) {
    m_backSide->setChecked(side == CardSide::Back);
    m_frontSide->setChecked(side == CardSide::Front);
    m_band->setEnabled(side == CardSide::Back);
    if (side == m_side) {
        return;
    }
    m_side = side;
    emit sideChanged(side);
    emit overlaysChanged(overlays());
}

void ReviewBar::reset() {
    {
        const QSignalBlocker blocker(this);
        showSide(CardSide::Back);
    }
    const ReviewOverlays defaults;
    for (auto [box, checked] : {std::pair{m_outline, defaults.outline}, std::pair{m_band, defaults.band},
                                std::pair{m_corners, defaults.corners}, std::pair{m_gates, defaults.gates}}) {
        const QSignalBlocker blocker(box);
        box->setChecked(checked);
    }
    emit overlaysChanged(overlays());
}

void ReviewBar::applyColours() {
    m_colours = reviewColours(darkAppearance());
    setTextColour(m_title, m_colours.text);
    setTextColour(m_lead, m_colours.secondary);
    for (QCheckBox* box : {m_outline, m_band, m_corners, m_gates}) {
        setTextColour(box, m_colours.text);
    }
    setTextColour(m_photoZone, m_colours.secondary);
    update();
}

void ReviewBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), m_colours.pillBackground);
}

void ReviewBar::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyColours();
    }
}

// ---- ReviewPanel ----

ReviewPanel::ReviewPanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(kPanelWidth);
    const QFont base = font();
    // Its contents scroll in a short window: the panel never makes the window taller, even hidden in the
    // results' place (a window that grew after it was shown drew its canvas again).
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(kPanelPadding, kPanelPadding, kPanelPadding, kPanelPadding);
    column->setSpacing(12);

    m_title = new QLabel(tr("MRZ"), this);
    m_title->setObjectName(QStringLiteral("mrzTitle"));
    m_title->setFont(pixelFont(base, kTitlePixels, QFont::Bold));
    column->addWidget(m_title);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    int index = 0;
    for (const RowSpec& spec : kRowSpecs) {
        Row row;
        const QString key = QString::fromLatin1(spec.key);
        row.label = new QLabel(tr(spec.label), this);
        row.label->setObjectName(key + QStringLiteral("Label"));
        row.value = new QLabel(this);
        row.value->setObjectName(key + QStringLiteral("Value"));
        row.value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row.check = new QLabel(this);
        row.check->setObjectName(key + QStringLiteral("Check"));
        row.check->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        for (QLabel* label : {row.label, row.value, row.check}) {
            label->setFont(pixelFont(base, kRowPixels));
        }
        grid->addWidget(row.label, index, 0);
        grid->addWidget(row.value, index, 1);
        grid->addWidget(row.check, index, 2);
        m_rows.push_back(row);
        ++index;
    }
    column->addLayout(grid);

    m_warning = new QLabel(this);
    m_warning->setObjectName(QStringLiteral("expiredWarning"));
    m_warning->setFont(pixelFont(base, kRowPixels));
    m_warning->setWordWrap(true);
    m_warning->hide();
    column->addWidget(m_warning);
    column->addStretch(1);

    const auto makeButton = [&](const char* name, const QString& text) {
        auto* made = new QPushButton(text, this);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(pixelFont(base, kButtonPixels, QFont::DemiBold));
        made->setCursor(Qt::PointingHandCursor);
        made->setAutoDefault(false);
        return made;
    };
    m_save = makeButton("saveButton", tr("Save to captures"));
    m_send = makeButton("sendButton", tr("Send to the viewer"));
    m_discard = makeButton("discardButton", tr("Discard"));
    m_scanAnother = makeButton("scanAnotherButton", tr("Scan another"));
    column->addWidget(m_save);
    column->addWidget(m_send);
    auto* pair = new QHBoxLayout;
    pair->setSpacing(8);
    pair->addWidget(m_discard, 1);
    pair->addWidget(m_scanAnother, 1);
    column->addLayout(pair);
    connect(m_save, &QPushButton::clicked, this, &ReviewPanel::save);
    connect(m_send, &QPushButton::clicked, this, &ReviewPanel::send);
    connect(m_discard, &QPushButton::clicked, this, &ReviewPanel::discard);
    connect(m_scanAnother, &QPushButton::clicked, this, &ReviewPanel::scanAnother);

    const auto failureLabel = [&](const char* name) {
        auto* made = new QLabel(this);
        made->setObjectName(QString::fromLatin1(name));
        made->setFont(pixelFont(base, kRowPixels));
        made->setWordWrap(true);
        made->hide();
        column->addWidget(made);
        return made;
    };
    m_saveFailure = failureLabel("saveFailure");
    m_sendFailure = failureLabel("sendFailure");

    m_note = new QLabel(tr("Nothing is saved or sent unless you choose it."), this);
    m_note->setObjectName(QStringLiteral("reviewNote"));
    m_note->setFont(pixelFont(base, kNotePixels));
    m_note->setWordWrap(true);
    column->addWidget(m_note);
    scroll->setWidget(content);
    // After setWidget, which turns it on: the content would paint the palette it started with over the
    // panel's card colour, so an appearance switch left it dark under light-mode text.
    content->setAutoFillBackground(false);
    applyColours();
}

void ReviewPanel::setScan(const lab_core::MrzScan& scan, doc_vision::MrzDate today) {
    const doc_vision::Mrz& mrz = scan.mrz;
    const auto birth = doc_vision::birthDateOf(mrz, today);
    const auto expiry = doc_vision::expiryDateOf(mrz, today);
    const QString values[] = {
        fieldText(mrz.surname),          fieldText(mrz.givenNames),     fieldText(mrz.documentNumber),
        fieldText(mrz.nationality),      mrzDate(mrz.birthDate, birth), fieldText(mrz.sex),
        mrzDate(mrz.expiryDate, expiry), fieldText(mrz.optionalData),   tr("check digit")};
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        Row& row = m_rows[i];
        // A4.1: TD1 and TD2 have optional data where TD3 has a personal number, with no check digit over it.
        if (std::string_view(kRowSpecs[i].key) == "personalNumber") {
            row.label->setText(mrz.format == doc_vision::MrzFormat::Td3 ? tr("Personal no.")
                                                                        : tr("Optional data"));
        }
        row.value->setText(values[i]);
        const auto check = kRowSpecs[i].check;
        const std::optional<bool> result = check ? passed(mrz, *check) : std::nullopt;
        row.failed = result.has_value() && !*result;
        row.check->setText(!result ? QString() : *result ? kCheckPassed : kCheckFailed);
    }
    const bool expired = expiry.has_value() && *expiry < today;
    m_warning->setText(expired ? expiredWarning(*expiry) : QString());
    m_warning->setVisible(expired);
    setSaveOutcome(Outcome::Idle);
    setSendOutcome(Outcome::Idle);
    applyColours();
}

void ReviewPanel::applyOutcome(QPushButton* button, QLabel* failureLabel, Outcome outcome,
                               const QString& failure, const QString& idle, const QString& busy,
                               const QString& done) {
    switch (outcome) {
    case Outcome::Idle:
    case Outcome::Failed:
        button->setText(idle);
        button->setEnabled(true);
        break;
    case Outcome::Busy:
        button->setText(busy);
        button->setEnabled(false);
        break;
    case Outcome::Done:
        button->setText(done);
        button->setEnabled(false);
        break;
    }
    const bool failed = outcome == Outcome::Failed;
    failureLabel->setText(failed ? failure : QString());
    failureLabel->setVisible(failed);
}

void ReviewPanel::setSaveOutcome(Outcome outcome, const QString& failure) {
    applyOutcome(m_save, m_saveFailure, outcome, failure, tr("Save to captures"), tr("Saving…"),
                 tr("Saved to captures ✓"));
}

void ReviewPanel::setSendOutcome(Outcome outcome, const QString& failure) {
    applyOutcome(m_send, m_sendFailure, outcome, failure, tr("Send to the viewer"), tr("Sending…"),
                 tr("Sent to the viewer ✓"));
}

void ReviewPanel::setSendAvailable(bool available) {
    if (!available) {
        m_send->setEnabled(false);
    }
}

void ReviewPanel::applyColours() {
    m_colours = reviewColours(darkAppearance());
    setTextColour(m_title, m_colours.text);
    for (const Row& row : m_rows) {
        setTextColour(row.label, m_colours.secondary);
        setTextColour(row.value, m_colours.text);
        setTextColour(row.check, row.failed ? m_colours.discardText : m_colours.title);
    }
    m_warning->setStyleSheet(
        QStringLiteral("QLabel { color: %1; background-color: %2; border-radius: 8px; padding: 10px 12px; }")
            .arg(m_colours.warnText.name(), m_colours.warnBackground.name()));
    m_save->setStyleSheet(buttonStyle(m_colours.primaryText, m_colours.primary, m_colours.primary));
    const QString plain = buttonStyle(m_colours.text, m_colours.cardBackground, m_colours.buttonBorder);
    m_send->setStyleSheet(plain);
    m_scanAnother->setStyleSheet(plain);
    m_discard->setStyleSheet(
        buttonStyle(m_colours.discardText, m_colours.discardBackground, m_colours.discardBorder));
    setTextColour(m_saveFailure, m_colours.discardText);
    setTextColour(m_sendFailure, m_colours.discardText);
    setTextColour(m_note, m_colours.secondary);
    update();
}

void ReviewPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), m_colours.cardBackground);
}

void ReviewPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyColours();
    }
}

// ---- GateValuesBox ----

GateValuesBox::GateValuesBox(QWidget* parent) : QWidget(parent) {
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(10, 8, 10, 8);
    m_layout->setSpacing(2);
}

void GateValuesBox::setRows(const std::vector<lab_core::Finding>& rows) {
    for (QLabel* label : m_labels) {
        delete label;
    }
    m_labels.clear();
    for (const lab_core::Finding& row : rows) {
        auto* label = new QLabel(QString::fromStdString(lab_core::toText(row)), this);
        label->setFont(pixelFont(font(), kRowPixels));
        // The canvas's overlays look the same in both appearances: the banner's colours.
        const QColor colour = row.tone == lab_core::Finding::Tone::Good ? kGuideGood
                              : row.tone == lab_core::Finding::Tone::Bad
                                  ? kGuideBad
                                  : bannerColours(doc_vision::Tone::Neutral).big;
        setTextColour(label, colour);
        m_layout->addWidget(label);
        // A4.1's side switch replaces the rows while the box shows: a child added then is shown only by a
        // queued call, and adjustSize() would size the box without it.
        label->show();
        m_labels.push_back(label);
    }
    adjustSize();
}

void GateValuesBox::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBannerPanel);
    painter.drawRoundedRect(QRectF(rect()), 8, 8);
}

} // namespace document_lab
