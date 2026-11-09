#pragma once

#include <QByteArray>
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
    QByteArray cameraId() const; // the camera used last
    void setCameraId(const QByteArray& id);
    QString formatFor(const QByteArray& cameraId) const; // the format label chosen for a camera
    void setFormatFor(const QByteArray& cameraId, const QString& label);
    QString labFolder() const; // chosen by the user; empty for the default
    void setLabFolder(const QString& folder);
    bool sendToViewer() const; // off unless turned on
    void setSendToViewer(bool on);
    QString endpoint() const; // visual_debug::defaultEndpoint() unless another was entered
    void setEndpoint(const QString& endpoint);
    int resultsWidth() const; // the results panel's, in pixels; 200 the first time
    void setResultsWidth(int width);
    bool resultsShown() const; // on unless View → Results turned it off
    void setResultsShown(bool shown);

private:
    QSettings m_settings;
};

} // namespace document_lab
