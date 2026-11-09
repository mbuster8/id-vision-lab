#include "lab_settings.h"

namespace document_lab {

namespace {

const QString kProcessor = QStringLiteral("lab/processor");
const QString kShow = QStringLiteral("lab/show");
const QString kOverlays = QStringLiteral("lab/overlays");
const QString kCamera = QStringLiteral("lab/camera");
const QString kFormats = QStringLiteral("lab/formats/");

} // namespace

QString LabSettings::processor() const { return m_settings.value(kProcessor).toString(); }

void LabSettings::setProcessor(const QString& name) { m_settings.setValue(kProcessor, name); }

QString LabSettings::show() const { return m_settings.value(kShow).toString(); }

void LabSettings::setShow(const QString& entry) { m_settings.setValue(kShow, entry); }

bool LabSettings::overlays() const { return m_settings.value(kOverlays, true).toBool(); }

void LabSettings::setOverlays(bool shown) { m_settings.setValue(kOverlays, shown); }

QByteArray LabSettings::cameraId() const { return m_settings.value(kCamera).toByteArray(); }

void LabSettings::setCameraId(const QByteArray& id) { m_settings.setValue(kCamera, id); }

// Camera ids may hold any byte, so each is a key in hex.
QString LabSettings::formatFor(const QByteArray& cameraId) const {
    return m_settings.value(kFormats + QString::fromLatin1(cameraId.toHex())).toString();
}

void LabSettings::setFormatFor(const QByteArray& cameraId, const QString& label) {
    m_settings.setValue(kFormats + QString::fromLatin1(cameraId.toHex()), label);
}

} // namespace document_lab
