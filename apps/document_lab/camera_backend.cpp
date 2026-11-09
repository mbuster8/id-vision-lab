#include "camera_backend.h"

#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QCoreApplication>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoSink>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif

#include <utility>

namespace document_lab {

namespace {

CameraFormat toCameraFormat(const QCameraFormat& format) {
    return {
        .size = format.resolution(), .maxFps = format.maxFrameRate(), .pixelFormat = format.pixelFormat()};
}

// Qt Multimedia's cameras: one QCamera, capture session and sink per start(), so nothing of an earlier start
// (a frame on its way, a late error) can be taken for this one's.
class QtCameraBackend final : public CameraBackend {
public:
    QtCameraBackend() {
        connect(&m_devices, &QMediaDevices::videoInputsChanged, this, [this] {
            emit camerasChanged();
            if (m_camera && !deviceWithId(m_runningId)) {
                stop();
                emit failed(tr("The camera is gone."));
            }
        });
    }

    ~QtCameraBackend() override { stop(); }

    std::vector<CameraInfo> cameras() const override {
        std::vector<CameraInfo> cameras;
        for (const QCameraDevice& device : QMediaDevices::videoInputs()) {
            CameraInfo info{.id = device.id(),
                            .name = device.description(),
                            .front = device.position() == QCameraDevice::FrontFace,
                            .formats = {}};
            for (const QCameraFormat& format : device.videoFormats()) {
                info.formats.push_back(toCameraFormat(format));
            }
            cameras.push_back(std::move(info));
        }
        return cameras;
    }

    void requestPermission(std::function<void(bool granted)> answer) override {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#if QT_CONFIG(permissions)
        const QCameraPermission permission;
        switch (qApp->checkPermission(permission)) {
        case Qt::PermissionStatus::Granted:
            answer(true);
            return;
        case Qt::PermissionStatus::Denied:
            answer(false);
            return;
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(permission, this, [answer = std::move(answer)](const QPermission& asked) {
                answer(asked.status() == Qt::PermissionStatus::Granted);
            });
            return;
        }
#endif
#endif
        answer(true); // before Qt 6.5, and where the platform asks nobody
    }

    quint64 start(const QByteArray& id, const CameraFormat& format) override {
        stop();
        const std::optional<QCameraDevice> device = deviceWithId(id);
        ++m_sessionNumber;
        if (!device) {
            const quint64 session = m_sessionNumber;
            QMetaObject::invokeMethod(
                this,
                [this, session] {
                    if (session == m_sessionNumber) {
                        emit failed(tr("The camera is gone."));
                    }
                },
                Qt::QueuedConnection);
            return m_sessionNumber;
        }
        m_runningId = id;
        m_camera = std::make_unique<QCamera>(*device);
        for (const QCameraFormat& candidate : device->videoFormats()) {
            if (toCameraFormat(candidate) == format) {
                m_camera->setCameraFormat(candidate);
                break;
            }
        }
        m_session = std::make_unique<QMediaCaptureSession>();
        m_sink = std::make_unique<QVideoSink>();
        m_session->setCamera(m_camera.get());
        m_session->setVideoSink(m_sink.get());
        const quint64 session = m_sessionNumber;
        connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
                [this, session](const QVideoFrame& frame) { emit frameArrived(frame, session); });
        connect(m_camera.get(), &QCamera::errorOccurred, this,
                [this, session](QCamera::Error error, const QString& reason) {
                    if (error != QCamera::NoError && session == m_sessionNumber) {
                        emit failed(reason.isEmpty() ? tr("The camera failed.") : reason);
                    }
                });
        m_camera->start();
        return session;
    }

    void stop() override {
        if (m_camera) {
            m_camera->stop();
        }
        // The sink first, so no frame comes while the rest goes.
        if (m_sink) {
            m_sink->disconnect(this);
        }
        if (m_camera) {
            m_camera->disconnect(this);
        }
        m_session.reset();
        m_sink.reset();
        m_camera.reset();
        m_runningId.clear();
    }

private:
    static std::optional<QCameraDevice> deviceWithId(const QByteArray& id) {
        for (const QCameraDevice& device : QMediaDevices::videoInputs()) {
            if (device.id() == id) {
                return device;
            }
        }
        return std::nullopt;
    }

    QMediaDevices m_devices;
    QByteArray m_runningId;
    quint64 m_sessionNumber = 0;
    std::unique_ptr<QCamera> m_camera;
    std::unique_ptr<QMediaCaptureSession> m_session;
    std::unique_ptr<QVideoSink> m_sink;
};

} // namespace

std::unique_ptr<CameraBackend> makeQtCameraBackend() { return std::make_unique<QtCameraBackend>(); }

} // namespace document_lab
