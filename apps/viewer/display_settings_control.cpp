#include "display_settings_control.h"

#include "display_text.h"

#include <QAction>
#include <QActionGroup>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace {

using viewer_core::ColourMap;
using viewer_core::DisplayRange;
using viewer_core::RangeMode;

std::size_t indexOf(RangeMode mode) { return static_cast<std::size_t>(mode); }
std::size_t indexOf(ColourMap colours) { return static_cast<std::size_t>(colours); }

QString rangeName(RangeMode mode) {
    switch (mode) {
    case RangeMode::Hold:
        return DisplaySettingsControl::tr("Hold");
    case RangeMode::Fixed:
        return DisplaySettingsControl::tr("Fixed");
    case RangeMode::Hdr:
        return QStringLiteral("HDR");
    case RangeMode::Auto:
        break;
    }
    return DisplaySettingsControl::tr("Auto");
}

QString coloursName(ColourMap colours) {
    switch (colours) {
    case ColourMap::Turbo:
        return QStringLiteral("Turbo");
    case ColourMap::Viridis:
        return QStringLiteral("Viridis");
    case ColourMap::Grey:
        break;
    }
    return DisplaySettingsControl::tr("Grey");
}

// HDR's own black and white, dragged on the histogram, as hdrBlackAndWhite takes them.
bool usable(const DisplayRange& range) {
    return std::isfinite(range.min) && std::isfinite(range.max) && range.min < range.max;
}

std::optional<double> parsed(const QString& text) {
    bool ok = false;
    const double value = QLocale::c().toDouble(text, &ok);
    return ok && std::isfinite(value) ? std::optional(value) : std::nullopt;
}

} // namespace

// Minimum and Maximum fields below the Range button. Apply is enabled only while the minimum is below the
// maximum; Enter applies, Esc and a click elsewhere close it unchanged.
class FixedRangePopup : public QFrame {
public:
    FixedRangePopup(QWidget* parent, std::function<void(DisplayRange)> apply)
        : QFrame(parent, Qt::Popup), m_minimum(new QLineEdit), m_maximum(new QLineEdit),
          m_apply(new QPushButton(DisplaySettingsControl::tr("Apply"))), m_onApply(std::move(apply)) {
        setObjectName(QStringLiteral("fixedRangePopup"));
        setFrameShape(QFrame::StyledPanel);
        m_minimum->setObjectName(QStringLiteral("fixedMinimum"));
        m_maximum->setObjectName(QStringLiteral("fixedMaximum"));
        m_apply->setObjectName(QStringLiteral("applyFixedRange"));
        m_apply->setDefault(true);
        for (QLineEdit* field : {m_minimum, m_maximum}) {
            auto* validator = new QDoubleValidator(field);
            validator->setLocale(QLocale::c());
            field->setValidator(validator);
            connect(field, &QLineEdit::textChanged, this, &FixedRangePopup::updateApply);
            connect(field, &QLineEdit::returnPressed, this, &FixedRangePopup::apply);
        }
        connect(m_apply, &QPushButton::clicked, this, &FixedRangePopup::apply);

        auto* fields = new QFormLayout;
        fields->addRow(DisplaySettingsControl::tr("Minimum"), m_minimum);
        fields->addRow(DisplaySettingsControl::tr("Maximum"), m_maximum);
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        buttons->addWidget(m_apply);
        auto* layout = new QVBoxLayout(this);
        layout->addLayout(fields);
        layout->addLayout(buttons);
    }

    // With its top-right corner at topRight, moved left or up as needed to stay within bounds (global).
    void open(const DisplayRange& range, const QPoint& topRight, const QRect& bounds) {
        m_minimum->setText(formattedNumber(range.min));
        m_maximum->setText(formattedNumber(range.max));
        updateApply();
        adjustSize();
        show(); // first, so that the frame, if the platform gives a popup one, counts in its size
        const QSize size = frameGeometry().size();
        QPoint position(topRight.x() - size.width() + 1, topRight.y());
        position.setX(std::max(bounds.left(), std::min(position.x(), bounds.right() - size.width() + 1)));
        position.setY(std::max(bounds.top(), std::min(position.y(), bounds.bottom() - size.height() + 1)));
        move(position);
        m_minimum->setFocus();
        m_minimum->selectAll();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    std::optional<DisplayRange> typedRange() const {
        const std::optional<double> minimum = parsed(m_minimum->text());
        const std::optional<double> maximum = parsed(m_maximum->text());
        if (!minimum || !maximum || *minimum >= *maximum) {
            return std::nullopt;
        }
        return DisplayRange{*minimum, *maximum};
    }

    void updateApply() { m_apply->setEnabled(typedRange().has_value()); }

    void apply() {
        if (const std::optional<DisplayRange> range = typedRange()) {
            hide();
            m_onApply(*range);
        }
    }

    QLineEdit* m_minimum;
    QLineEdit* m_maximum;
    QPushButton* m_apply;
    std::function<void(DisplayRange)> m_onApply;
};

DisplaySettingsControl::DisplaySettingsControl(QWidget* parent)
    : QWidget(parent), m_rangeButton(new QToolButton), m_coloursButton(new QToolButton),
      m_rangeMenu(new QMenu(tr("Range"), this)), m_coloursMenu(new QMenu(tr("Colours"), this)) {
    auto* ranges = new QActionGroup(this);
    for (const RangeMode mode : {RangeMode::Auto, RangeMode::Hold, RangeMode::Fixed, RangeMode::Hdr}) {
        QAction* action = ranges->addAction(mode == RangeMode::Fixed ? tr("Fixed…") : rangeName(mode));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, mode] { chooseRange(mode); });
        m_rangeMenu->addAction(action);
        m_rangeActions[indexOf(mode)] = action;
    }
    m_rangeActions[indexOf(RangeMode::Hold)]->setToolTip(tr("Keep the current range for the frames to come"));
    m_rangeActions[indexOf(RangeMode::Hdr)]->setToolTip(tr("Tone-map HLG or PQ for this screen"));
    m_rangeActions[indexOf(RangeMode::Hdr)]->setVisible(false); // until an HDR image is shown
    auto* colourMaps = new QActionGroup(this);
    for (const ColourMap colours : {ColourMap::Grey, ColourMap::Turbo, ColourMap::Viridis}) {
        QAction* action = colourMaps->addAction(coloursName(colours));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, colours] { chooseColours(colours); });
        m_coloursMenu->addAction(action);
        m_coloursActions[indexOf(colours)] = action;
    }

    m_rangeButton->setObjectName(QStringLiteral("rangeButton"));
    m_coloursButton->setObjectName(QStringLiteral("coloursButton"));
    // The ▾ is part of the text, and the menu pops up on a click, rather than given to the button: some
    // styles (Fusion among them) draw a menu button's arrow over the end of its text.
    for (const auto& [button, menu] :
         {std::pair{m_rangeButton, m_rangeMenu}, std::pair{m_coloursButton, m_coloursMenu}}) {
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(button, &QToolButton::clicked, menu,
                [button, menu] { menu->popup(button->mapToGlobal(QPoint(0, button->height()))); });
    }
    m_coloursButton->setToolTip(tr("Colour map for single-channel images"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_rangeButton);
    layout->addWidget(m_coloursButton);
    showSettings();
}

void DisplaySettingsControl::setState(const viewer_core::DisplaySettings& settings, bool singleChannel,
                                      bool hdrAvailable) {
    if (m_popup != nullptr) {
        m_popup->hide(); // it was for the picture shown before
    }
    m_settings = settings;
    setFrameKind(singleChannel, hdrAvailable);
}

void DisplaySettingsControl::setFrameKind(bool singleChannel, bool hdrAvailable) {
    m_coloursButton->setVisible(singleChannel);
    for (QAction* action : m_coloursActions) {
        action->setEnabled(singleChannel);
    }
    m_hdrAvailable = hdrAvailable;
    m_rangeActions[indexOf(RangeMode::Hdr)]->setVisible(hdrAvailable);
    showSettings(); // HDR settings read as Auto on a frame without an HDR tag
}

void DisplaySettingsControl::setAutoRangeProvider(std::function<viewer_core::DisplayRange()> provider) {
    m_autoRange = std::move(provider);
}

QMenu* DisplaySettingsControl::rangeMenu() const { return m_rangeMenu; }

QMenu* DisplaySettingsControl::coloursMenu() const { return m_coloursMenu; }

const viewer_core::DisplaySettings& DisplaySettingsControl::settings() const { return m_settings; }

void DisplaySettingsControl::applySettings(const viewer_core::DisplaySettings& settings) {
    if (m_popup != nullptr) {
        m_popup->hide(); // it was typed for the settings these replace
    }
    m_settings = settings;
    showSettings();
    emit settingsChanged(m_settings);
}

void DisplaySettingsControl::chooseRange(RangeMode mode) {
    switch (mode) {
    case RangeMode::Auto:
    case RangeMode::Hdr:
        m_settings.mode = mode;
        m_settings.range = {};
        break;
    case RangeMode::Hold:
        // The range Auto shows now, whatever the mode was.
        m_settings.mode = RangeMode::Hold;
        m_settings.range = m_autoRange ? m_autoRange() : m_settings.range;
        break;
    case RangeMode::Fixed:
        openFixedPopup();
        showSettings(); // the tick stays where it was until Apply
        return;
    }
    showSettings();
    emit settingsChanged(m_settings);
}

void DisplaySettingsControl::chooseColours(ColourMap colours) {
    m_settings.colours = colours;
    showSettings();
    emit settingsChanged(m_settings);
}

void DisplaySettingsControl::openFixedPopup() {
    if (m_popup == nullptr) {
        m_popup = new FixedRangePopup(this, [this](DisplayRange range) { applyFixed(range); });
    }
    // Right-aligned under the Range button, which sits at the info strip's right end, and inside the window
    // and the screen.
    QRect bounds = window()->geometry(); // the window's content, without its frame
    if (const QScreen* screen = m_rangeButton->screen()) {
        bounds &= screen->availableGeometry();
    }
    m_popup->open(currentRange(),
                  m_rangeButton->mapToGlobal(QPoint(m_rangeButton->width() - 1, m_rangeButton->height())),
                  bounds);
}

void DisplaySettingsControl::applyFixed(DisplayRange range) {
    m_settings.mode = RangeMode::Fixed;
    m_settings.range = range;
    showSettings();
    emit settingsChanged(m_settings);
}

void DisplaySettingsControl::showSettings() {
    // What the picture shows: HDR settings on an image without an HDR tag show it as Auto.
    const RangeMode shown =
        m_settings.mode == RangeMode::Hdr && !m_hdrAvailable ? RangeMode::Auto : m_settings.mode;
    m_rangeButton->setText(tr("Range: %1 ▾").arg(rangeName(shown)));
    switch (shown) {
    case RangeMode::Auto:
        m_rangeButton->setToolTip(tr("Each picture from its own minimum to its maximum"));
        break;
    case RangeMode::Hdr:
        m_rangeButton->setToolTip(
            usable(m_settings.range)
                ? tr("HLG or PQ tone-mapped for this screen, black at %1 and white at %2")
                      .arg(formattedNumber(m_settings.range.min), formattedNumber(m_settings.range.max))
                : tr("HLG or PQ tone-mapped for this screen"));
        break;
    case RangeMode::Hold:
    case RangeMode::Fixed:
        m_rangeButton->setToolTip(
            tr("%1 to %2").arg(formattedNumber(m_settings.range.min), formattedNumber(m_settings.range.max)));
        break;
    }
    m_coloursButton->setText(tr("Colours: %1 ▾").arg(coloursName(m_settings.colours)));
    m_rangeActions[indexOf(shown)]->setChecked(true);
    m_coloursActions[indexOf(m_settings.colours)]->setChecked(true);
}

// Auto's range for Auto, and for HDR unless its black and white were dragged on the histogram.
DisplayRange DisplaySettingsControl::currentRange() const {
    if (m_settings.mode == RangeMode::Hold || m_settings.mode == RangeMode::Fixed) {
        return m_settings.range;
    }
    if (m_settings.mode == RangeMode::Hdr && m_hdrAvailable && usable(m_settings.range)) {
        return m_settings.range;
    }
    return m_autoRange ? m_autoRange() : DisplayRange{};
}
