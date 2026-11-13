#pragma once

#include <QWidget>

class QEvent;
class QLabel;
class QPaintEvent;
class QPushButton;
class QScrollArea;

// Declared, not included: lab_core/document_kind.h brings doc_vision/mrz.h, whose <compare> is then the first
// standard header Qt 6.4's moc meets here, and it cannot parse GCC 13's <concepts> behind it (Ubuntu 24.04).
namespace lab_core {
enum class DocumentKind;
} // namespace lab_core

namespace document_lab {

// Screen 04 (A4, spec §3.3): over the canvas, the captured frame dimmed and a card that says the passport is
// scanned and to place it on the chip reader, with Skip the chip and Scan again. At most 620 px wide and 16
// px clear of the canvas's sides; on a smaller canvas its texts wrap and it scrolls, so its buttons stay
// reachable. Its colours follow the appearance.
class ScannedCard : public QWidget {
    Q_OBJECT

public:
    explicit ScannedCard(QWidget* parent = nullptr);

    // A4.1: the words and the drawing for what the accepted MRZ says the document is; a passport's until set.
    void setKind(lab_core::DocumentKind kind);

signals:
    void skipChip();
    void scanAgain();

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void applyColours();

    QScrollArea* m_scroll = nullptr;
    QWidget* m_body = nullptr;
    QWidget* m_check = nullptr;
    QWidget* m_illustration = nullptr;
    QWidget* m_pill = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_headline = nullptr;
    QLabel* m_explanation = nullptr;
    QLabel* m_readerLine = nullptr;
    QLabel* m_note = nullptr;
    QPushButton* m_skipChip = nullptr;
    QPushButton* m_scanAgain = nullptr;
};

} // namespace document_lab
