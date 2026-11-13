#pragma once

#include "scan_style.h"

#include <QWidget>

class QEvent;
class QLabel;
class QPaintEvent;
class QPushButton;

namespace document_lab {

// A4.1, spec §6.6: above the canvas, in Review's bar's place, while a TD1 card's front is looked for: the
// back is scanned; Skip the front, or Scan again. On a replay, it says to open the front's clip (D11). Its
// colours follow the appearance, as Review's bar's do.
class TurnOverBar : public QWidget {
    Q_OBJECT

public:
    explicit TurnOverBar(QWidget* parent = nullptr);

    // The back was accepted on a replay, which cannot show the other side: the front comes from another clip.
    void setBackOnReplay(bool onReplay);

signals:
    void skipFront();
    void scanAgain();

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void applyColours();

    ReviewColours m_colours;
    QLabel* m_title = nullptr;
    QPushButton* m_skipFront = nullptr;
    QPushButton* m_scanAgain = nullptr;
};

} // namespace document_lab
