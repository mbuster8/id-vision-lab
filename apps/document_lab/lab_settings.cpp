#include "lab_settings.h"

namespace document_lab {

namespace {

const QString kProcessor = QStringLiteral("lab/processor");
const QString kShow = QStringLiteral("lab/show");
const QString kOverlays = QStringLiteral("lab/overlays");

} // namespace

QString LabSettings::processor() const { return m_settings.value(kProcessor).toString(); }

void LabSettings::setProcessor(const QString& name) { m_settings.setValue(kProcessor, name); }

QString LabSettings::show() const { return m_settings.value(kShow).toString(); }

void LabSettings::setShow(const QString& entry) { m_settings.setValue(kShow, entry); }

bool LabSettings::overlays() const { return m_settings.value(kOverlays, true).toBool(); }

void LabSettings::setOverlays(bool shown) { m_settings.setValue(kOverlays, shown); }

} // namespace document_lab
