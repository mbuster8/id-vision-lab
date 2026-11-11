#pragma once

// The camera focus spike: a camera's own focus controls, reached around Qt, which offers none of them for a UVC
// camera on macOS. On macOS a UVC camera's Focus, Auto and Focus (Absolute) through IOKit; on Windows the
// driver's focus through DirectShow's IAMCameraControl; elsewhere nothing is found.

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

namespace uvc {

struct FocusRange {
    int minimum = 0;
    int maximum = 0;
    int resolution = 0;
    int byDefault = 0;
};

class Camera {
public:
    virtual ~Camera() = default;
    // The camera Qt Multimedia names `qtId` (on macOS "0x" and the USB location, vendor and product in hex);
    // nothing when it is not a UVC camera this platform can reach.
    static std::unique_ptr<Camera> forQtId(const QByteArray& qtId, QString& why);

    virtual QString summary() = 0; // what the camera says about its focus controls, read only
    virtual bool hasAutoFocus() const = 0;
    virtual bool hasAbsoluteFocus() const = 0;
    virtual std::optional<bool> autoFocus() = 0;
    virtual bool setAutoFocus(bool on) = 0;
    virtual std::optional<FocusRange> focusRange() = 0;
    virtual std::optional<int> focus() = 0;
    virtual bool setFocus(int position) = 0;
};

} // namespace uvc
