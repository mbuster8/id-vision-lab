#include "camera_backend.h"

#include <QCamera>
#include <QCameraDevice>
#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

// A camera's error reaches the window only once the camera's signal has returned: the window stops the camera
// in answer, which deletes it, and Qt goes on using the camera after the signal's slots return (a use after
// free when a camera in use elsewhere, or refused by the system, fails).
TEST(CameraBackend, ErrorIsReportedOnceTheSignalReturns) {
    QObject context;
    auto camera = std::make_unique<QCamera>(QCameraDevice{});
    std::vector<QString> reported;
    document_lab::reportCameraErrorsLater(*camera, context,
                                          [&reported](const QString& reason) { reported.push_back(reason); });

    emit camera->errorOccurred(QCamera::CameraError, QStringLiteral("The camera is in use."));

    EXPECT_TRUE(reported.empty());
    camera.reset(); // as stopping the camera in answer does
    QCoreApplication::sendPostedEvents(&context);
    EXPECT_EQ(reported, std::vector<QString>{QStringLiteral("The camera is in use.")});
}

TEST(CameraBackend, ErrorWithoutAReasonGetsOne) {
    QObject context;
    QCamera camera{QCameraDevice{}};
    std::vector<QString> reported;
    document_lab::reportCameraErrorsLater(camera, context,
                                          [&reported](const QString& reason) { reported.push_back(reason); });

    emit camera.errorOccurred(QCamera::NoError, QString());
    emit camera.errorOccurred(QCamera::CameraError, QString());
    QCoreApplication::sendPostedEvents(&context);

    EXPECT_EQ(reported, std::vector<QString>{QStringLiteral("The camera failed.")});
}

} // namespace
