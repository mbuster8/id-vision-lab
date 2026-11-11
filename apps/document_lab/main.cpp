#include "appearance.h"
#include "application_identity.h"
#include "camera_backend.h"
#include "camera_focus.h"
#include "main_window.h"

#include "visual_debug/client.h"

#include <QApplication>
#include <QIcon>

#include <memory>
#include <string>
#include <utility>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    qt_common::setApplicationIdentity(QLatin1String(qt_common::kDocumentLabName));
    qt_common::applyAppearance(qt_common::storedAppearance());
    QApplication::setApplicationVersion(QStringLiteral(ID_VISION_LAB_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/document_lab_icon.png")));

    document_lab::LabDependencies dependencies;
    dependencies.cameras = document_lab::makeQtCameraBackend();
    dependencies.makeFocusControl = document_lab::makeFocusControl;
    dependencies.makeSender = [](const std::string& endpoint) -> std::unique_ptr<visual_debug::ImageSender> {
        return std::make_unique<visual_debug::Client>(visual_debug::ClientOptions{.endpoint = endpoint});
    };
    document_lab::LabWindow window(std::move(dependencies));
    window.resize(1280, 860);
    window.show();
    return QApplication::exec();
}
