#pragma once

// Cameras for the Document Lab's window tests: a list the test sets, a permission it answers, and frames it
// delivers to a session of its choosing.

#include "camera_backend.h"

#include <QVideoFrame>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace test_support {

class FakeCameraBackend : public document_lab::CameraBackend {
public:
    std::vector<document_lab::CameraInfo> cameras() const override { return list; }

    void requestPermission(std::function<void(bool granted)> answer) override {
        ++permissionRequests;
        if (permission) {
            answer(*permission);
        } else {
            pendingAnswer = std::move(answer);
        }
    }

    void start(const QByteArray& id, const document_lab::CameraFormat& format) override {
        starts.push_back({id, format});
        ++session;
        running = true;
    }

    void stop() override {
        ++stops;
        running = false;
    }

    // A frame of the current session, or of `of`.
    void deliver(const QVideoFrame& frame) { emit frameArrived(frame, session); }
    void deliver(const QVideoFrame& frame, quint64 of) { emit frameArrived(frame, of); }
    void fail(const QString& reason) { emit failed(reason); }
    void changeCameras(std::vector<document_lab::CameraInfo> cameras) {
        list = std::move(cameras);
        emit camerasChanged();
    }

    std::vector<document_lab::CameraInfo> list;
    std::optional<bool> permission = true; // nothing: the answer waits in pendingAnswer
    std::function<void(bool)> pendingAnswer;
    int permissionRequests = 0;
    std::vector<std::pair<QByteArray, document_lab::CameraFormat>> starts;
    int stops = 0;
    quint64 session = 0;
    bool running = false;
};

} // namespace test_support
