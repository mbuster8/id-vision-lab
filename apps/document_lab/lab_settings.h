#pragma once

#include <QSettings>
#include <QString>

namespace document_lab {

// What the lab remembers between runs, in the application's own settings.
class LabSettings {
public:
    QString processor() const; // a processor's name, as lab_core::nameOf gives it
    void setProcessor(const QString& name);
    QString show() const; // the Show picker's entry
    void setShow(const QString& entry);
    bool overlays() const; // on unless turned off
    void setOverlays(bool shown);

private:
    QSettings m_settings;
};

} // namespace document_lab
