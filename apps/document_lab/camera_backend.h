#pragma once

#include <QByteArray>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <vector>

namespace document_lab {

// One way a camera can deliver frames.
struct CameraFormat {
    QSize size;
    float maxFps = 0;
    QVideoFrameFormat::PixelFormat pixelFormat = QVideoFrameFormat::Format_Invalid;

    QString label() const; // "1920×1440, 60 fps, NV12"
    bool operator==(const CameraFormat&) const = default;
};

struct CameraInfo {
    QByteArray id;
    QString name;
    bool front = false;
    std::vector<CameraFormat> formats;
};

// The cameras, behind an interface so the window's tests can use fake ones. Its signals come on the thread
// the backend lives on.
class CameraBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual std::vector<CameraInfo> cameras() const = 0;
    // The answer comes once, at once when the platform asks nobody.
    virtual void requestPermission(std::function<void(bool granted)> answer) = 0;
    // A new session each time, with a number of its own: frames of an earlier start never belong to it.
    // Returns the session's number, which its frames carry.
    virtual quint64 start(const QByteArray& id, const CameraFormat& format) = 0;
    virtual void stop() = 0;

signals:
    void camerasChanged();
    // `session` is the number of the start() it belongs to.
    void frameArrived(const QVideoFrame& frame, quint64 session);
    // The camera failed or went away; its session is over.
    void failed(const QString& reason);
};

// Qt Multimedia's cameras: QMediaDevices' list, a new QCamera, capture session and QVideoSink per start(),
// and the platform's camera permission from Qt 6.5.
std::unique_ptr<CameraBackend> makeQtCameraBackend();

} // namespace document_lab
