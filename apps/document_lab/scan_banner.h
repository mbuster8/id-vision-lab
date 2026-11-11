#pragma once

#include "scan_words.h"

#include <doc_vision/guidance.h>

#include <QFont>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <optional>

class QPaintEvent;

namespace document_lab {

// The banner's big line as drawn: the pixel size it fits at, and its text, elided when even 22 px is too
// wide.
struct FittedLine {
    int pixelSize = 0;
    QString text;
};

// `base` at 32 px in the heaviest weight the platform's font offers: ExtraBold or Black, else Bold.
QFont bigLineFont(const QFont& base);
// The largest pixel size from `font`'s down to 22 at which `text` fits `availableWidth`; at 22, elided.
FittedLine fitBigLine(QFont font, const QString& text, int availableWidth);

// The scan banner over the canvas's top centre (docs/images/scan-flow/01, 02): a dark panel with an icon, a
// big line and a small one. It carries its 28 px gap from the canvas's top as transparent margin, and keeps
// 16 px clear of each side. Transparent to the mouse, so zoom, pan and the pixel readout work beneath.
class ScanBanner : public QWidget {
    Q_OBJECT

public:
    explicit ScanBanner(QWidget* parent = nullptr);

    void setContent(std::optional<BannerContent> content); // nullopt hides it
    std::optional<BannerContent> content() const { return m_content; }
    FittedLine fittedBigLine() const { return m_fitted; } // as last painted

    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Layout {
        FittedLine big;
        QRectF panel;     // in the widget
        QRectF smallText; // in the widget
        int bigWidth = 0; // the fitted big line's advance
        int rowHeight = 0;
    };
    Layout layoutFor(int width) const;

    std::optional<BannerContent> m_content;
    FittedLine m_fitted;
};

// The six gate chips over the canvas's bottom centre: one dark pill of ✓ and ✗ with each gate's name,
// wrapping onto more rows when narrow. It carries its 22 px gap from the canvas's bottom as transparent
// margin. Transparent to the mouse.
class GateChips : public QWidget {
    Q_OBJECT

public:
    explicit GateChips(QWidget* parent = nullptr);

    void setStates(const doc_vision::ChipStates& states); // hidden while !states.visible
    doc_vision::ChipStates states() const { return m_states; }

    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QFont chipFont() const;

    doc_vision::ChipStates m_states;
};

} // namespace document_lab
