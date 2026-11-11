#include "model_folder.h"

#include <QCoreApplication>
#include <QDir>
#include <QtGlobal>

namespace document_lab {

QString defaultModelFolder() {
    const QDir program(QCoreApplication::applicationDirPath());
#if defined(Q_OS_MACOS)
    return QDir::cleanPath(program.filePath(QStringLiteral("../Resources/models")));
#else
    return QDir::cleanPath(program.filePath(QStringLiteral("models")));
#endif
}

} // namespace document_lab
