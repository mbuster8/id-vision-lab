#pragma once

#include "scan_style.h"

#include <doc_vision/mrz.h>
#include <lab_core/frame_processor.h>

#include <QMetaType>
#include <QString>
#include <QWidget>

#include <vector>

class QCheckBox;
class QEvent;
class QLabel;
class QPaintEvent;
class QPushButton;
class QVBoxLayout;

namespace document_lab {

// What Review draws over the captured frame (spec §3.4): the check boxes' states.
struct ReviewOverlays {
    bool outline = true;
    bool band = true;
    bool corners = false;
    bool gates = false;
    bool operator==(const ReviewOverlays&) const = default;
};

// Review's bar above the canvas: "Captured frame", "Show over it:" and its five check boxes, the photo zone's
// disabled until A3.2. Follows the appearance.
class ReviewBar : public QWidget {
    Q_OBJECT

public:
    explicit ReviewBar(QWidget* parent = nullptr);

    ReviewOverlays overlays() const;
    // Back to the defaults, with one overlaysChanged: each Review starts from them.
    void reset();

signals:
    void overlaysChanged(document_lab::ReviewOverlays overlays);

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void applyColours();

    ReviewColours m_colours;
    QLabel* m_title = nullptr;
    QLabel* m_lead = nullptr;
    QCheckBox* m_outline = nullptr;
    QCheckBox* m_band = nullptr;
    QCheckBox* m_corners = nullptr;
    QCheckBox* m_gates = nullptr;
    QCheckBox* m_photoZone = nullptr;
};

// Review's panel in the results' place, at least 330 px wide: the MRZ's fields and check digits, the expiry
// warning, Save, Send, Discard and Scan another, and what became of a save or a send. Follows the appearance.
class ReviewPanel : public QWidget {
    Q_OBJECT

public:
    enum class Outcome { Idle, Busy, Done, Failed };

    explicit ReviewPanel(QWidget* parent = nullptr);

    // Shows `scan`'s MRZ; dates in the century `today` puts them in, and the warning when it expired before
    // `today`. Both outcomes go back to Idle.
    void setScan(const lab_core::MrzScan& scan, doc_vision::MrzDate today);
    // Done disables the button and says so on it; Failed shows `failure` under the buttons and enables it
    // again.
    void setSaveOutcome(Outcome outcome, const QString& failure = {});
    void setSendOutcome(Outcome outcome, const QString& failure = {});

signals:
    void save();
    void send();
    void discard();
    void scanAnother();

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Row {
        QLabel* label = nullptr;
        QLabel* value = nullptr;
        QLabel* check = nullptr;
        bool failed = false; // its check digit failed
    };
    void applyColours();
    void applyOutcome(QPushButton* button, QLabel* failureLabel, Outcome outcome, const QString& failure,
                      const QString& idle, const QString& busy, const QString& done);

    ReviewColours m_colours;
    QLabel* m_title = nullptr;
    std::vector<Row> m_rows;
    QLabel* m_warning = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_send = nullptr;
    QPushButton* m_discard = nullptr;
    QPushButton* m_scanAnother = nullptr;
    QLabel* m_saveFailure = nullptr;
    QLabel* m_sendFailure = nullptr;
    QLabel* m_note = nullptr;
};

// The captured frame's gate values, top left on the canvas under Review's pill: one "Name: value" line per
// row, on the banner's dark panel in both appearances.
class GateValuesBox : public QWidget {
public:
    explicit GateValuesBox(QWidget* parent = nullptr);
    void setRows(const std::vector<lab_core::Finding>& rows);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVBoxLayout* m_layout = nullptr;
    std::vector<QLabel*> m_labels;
};

} // namespace document_lab

Q_DECLARE_METATYPE(document_lab::ReviewOverlays)
