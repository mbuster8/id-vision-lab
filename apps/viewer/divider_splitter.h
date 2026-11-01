#pragma once

#include <QSize>
#include <QSplitter>

class QPainter;
class QSplitterHandle;

// A divider handle's look in a widget of `size`: between widgets side by side (a vertical line), or between
// rows (a horizontal one).
void paintDivider(QPainter& painter, QSize size, bool sideBySide);

// A splitter whose handles show where to grab them: on the panel grey, a 1-point divider line along the
// middle with a short grip mark at its centre, in the divider grey (spec H3).
class DividerSplitter : public QSplitter {
public:
    static constexpr int kHandleWidth = 7;

    explicit DividerSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    QSplitterHandle* createHandle() override;
};
