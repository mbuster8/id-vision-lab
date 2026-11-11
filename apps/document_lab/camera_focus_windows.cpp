// The third file to include <windows.h> (through <dshow.h>), allowed for DirectShow's focus control (A3.3,
// owner's decision D4). It includes nothing of the project's but camera_focus.h and lab_core's
// focus_control.h: <windows.h> brings wingdi's Polygon, which clashes with the project's.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "camera_focus.h"

#include <QString>

#include <dshow.h>

#include <optional>

namespace document_lab {

namespace {

// A device path without its interface class: Qt names a camera by its Media Foundation link and DirectShow by
// its own, and the two differ only after the last "#{".
QString stem(QString path) {
    path = path.toLower();
    const qsizetype at = path.lastIndexOf(QStringLiteral("#{"));
    return at > 0 ? path.left(at) : path;
}

// A driver's focus through DirectShow's IAMCameraControl (CameraControl_Focus). Opened on its first call, on
// the processing thread, where COM is initialised in the multithreaded apartment and uninitialised again when
// the control goes. The API has no time limit: a driver that blocks holds the processing thread (spec §13).
class WindowsFocusControl final : public lab_core::FocusControl {
public:
    explicit WindowsFocusControl(QString path) : m_path(std::move(path)) {}
    ~WindowsFocusControl() override {
        if (m_control != nullptr) {
            m_control->Release();
        }
        if (m_uninitialise) {
            CoUninitialize();
        }
    }
    WindowsFocusControl(const WindowsFocusControl&) = delete;
    WindowsFocusControl& operator=(const WindowsFocusControl&) = delete;

    std::optional<lab_core::FocusRange> range() noexcept override {
        if (!open() || (m_caps & CameraControl_Flags_Manual) == 0 || m_max <= m_min) {
            return std::nullopt;
        }
        return lab_core::FocusRange{.minimum = static_cast<int>(m_min),
                                    .maximum = static_cast<int>(m_max),
                                    .step = m_step >= 1 ? static_cast<int>(m_step) : 1};
    }
    bool hasAutoFocus() noexcept override { return open() && (m_caps & CameraControl_Flags_Auto) != 0; }
    std::optional<bool> autoFocus() noexcept override {
        long value = 0;
        long flags = 0;
        if (!hasAutoFocus() || FAILED(m_control->Get(CameraControl_Focus, &value, &flags))) {
            return std::nullopt;
        }
        return (flags & CameraControl_Flags_Auto) != 0;
    }
    bool setAutoFocus(bool on) noexcept override {
        if (!hasAutoFocus()) {
            return false;
        }
        long value = m_default;
        long flags = 0;
        if (FAILED(m_control->Get(CameraControl_Focus, &value, &flags))) {
            value = m_default;
        }
        return SUCCEEDED(m_control->Set(CameraControl_Focus, value,
                                        on ? CameraControl_Flags_Auto : CameraControl_Flags_Manual));
    }
    std::optional<int> position() noexcept override {
        long value = 0;
        long flags = 0;
        if (!open() || FAILED(m_control->Get(CameraControl_Focus, &value, &flags))) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    }
    bool setPosition(int position) noexcept override {
        return open() && SUCCEEDED(m_control->Set(CameraControl_Focus, position, CameraControl_Flags_Manual));
    }

private:
    bool open() noexcept {
        if (m_control != nullptr) {
            return true;
        }
        if (m_tried) {
            return false;
        }
        m_tried = true;
        try {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            m_uninitialise = SUCCEEDED(com); // RPC_E_CHANGED_MODE: usable, and not ours to end
            find();
        } catch (...) {
            // Qt's strings may throw on allocation: no control, then.
        }
        return m_control != nullptr;
    }

    void find() {
        ICreateDevEnum* devices = nullptr;
        IEnumMoniker* monikers = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&devices))) &&
            devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK) {
            const QString wanted = stem(m_path);
            IMoniker* moniker = nullptr;
            while (m_control == nullptr && monikers->Next(1, &moniker, nullptr) == S_OK) {
                IPropertyBag* bag = nullptr;
                if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                    VARIANT path;
                    VariantInit(&path);
                    if (SUCCEEDED(bag->Read(L"DevicePath", &path, nullptr)) && path.vt == VT_BSTR &&
                        stem(QString::fromWCharArray(path.bstrVal)) == wanted) {
                        IBaseFilter* filter = nullptr;
                        IAMCameraControl* control = nullptr;
                        if (SUCCEEDED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))) &&
                            SUCCEEDED(filter->QueryInterface(IID_PPV_ARGS(&control)))) {
                            if (SUCCEEDED(control->GetRange(CameraControl_Focus, &m_min, &m_max, &m_step,
                                                            &m_default, &m_caps))) {
                                m_control = control;
                            } else {
                                control->Release();
                            }
                        }
                        if (filter != nullptr) {
                            filter->Release();
                        }
                    }
                    VariantClear(&path);
                    bag->Release();
                }
                moniker->Release();
            }
        }
        if (monikers != nullptr) {
            monikers->Release();
        }
        if (devices != nullptr) {
            devices->Release();
        }
    }

    QString m_path;
    bool m_tried = false;
    bool m_uninitialise = false;
    IAMCameraControl* m_control = nullptr;
    long m_min = 0;
    long m_max = 0;
    long m_step = 0;
    long m_default = 0;
    long m_caps = 0;
};

} // namespace

std::unique_ptr<lab_core::FocusControl> makeFocusControl(const QByteArray& qtCameraId) {
    if (!qtCameraId.startsWith(R"(\\?\)")) {
        return nullptr;
    }
    return std::make_unique<WindowsFocusControl>(QString::fromUtf8(qtCameraId));
}

} // namespace document_lab
