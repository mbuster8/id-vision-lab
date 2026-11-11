#include "uvc_controls.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <dshow.h>
#endif

#include <QStringList>

#include <array>
#include <vector>

namespace uvc {

#if defined(__APPLE__)

namespace {

// UVC 1.1, A.8 and A.9.4.
constexpr std::uint8_t kSetCur = 0x01;
constexpr std::uint8_t kGetCur = 0x81;
constexpr std::uint8_t kGetMin = 0x82;
constexpr std::uint8_t kGetMax = 0x83;
constexpr std::uint8_t kGetRes = 0x84;
constexpr std::uint8_t kGetDef = 0x87;
constexpr std::uint8_t kFocusAbsolute = 0x06;
constexpr std::uint8_t kFocusAuto = 0x08;

std::optional<std::uint32_t> numberProperty(io_service_t service, const char* name) {
    const CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    const CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    CFRelease(key);
    if (!value) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> result;
    std::int64_t number = 0;
    if (CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number)) {
        result = static_cast<std::uint32_t>(number);
    }
    CFRelease(value);
    return result;
}

class MacCamera final : public Camera {
public:
    MacCamera(IOUSBDeviceInterface** device, std::uint8_t interfaceNumber, std::uint8_t terminal,
              std::uint32_t controls)
        : m_device(device), m_interface(interfaceNumber), m_terminal(terminal), m_controls(controls) {}
    ~MacCamera() override { (*m_device)->Release(m_device); }
    MacCamera(const MacCamera&) = delete;
    MacCamera& operator=(const MacCamera&) = delete;

    QString summary() override {
        QStringList lines;
        lines << QStringLiteral("    UVC: VideoControl interface %1, camera terminal %2, controls 0x%3")
                     .arg(m_interface)
                     .arg(m_terminal)
                     .arg(m_controls, 6, 16, QLatin1Char('0'));
        lines << QStringLiteral("    UVC Focus, Auto: %1%2")
                     .arg(hasAutoFocus() ? QStringLiteral("offered") : QStringLiteral("not offered"))
                     .arg(autoFocus()
                              ? (*autoFocus() ? QStringLiteral(", now on") : QStringLiteral(", now off"))
                              : QStringLiteral(", unreadable (%1)").arg(lastError()));
        const std::optional<FocusRange> range = focusRange();
        const std::optional<int> now = focus();
        lines << QStringLiteral("    UVC Focus (Absolute): %1%2")
                     .arg(hasAbsoluteFocus() ? QStringLiteral("offered") : QStringLiteral("not offered"))
                     .arg(range && now ? QStringLiteral(", %1 to %2 by %3, default %4, now %5")
                                             .arg(range->minimum)
                                             .arg(range->maximum)
                                             .arg(range->resolution)
                                             .arg(range->byDefault)
                                             .arg(*now)
                                       : QStringLiteral(", unreadable (%1)").arg(lastError()));
        return lines.join(QLatin1Char('\n'));
    }
    bool hasAutoFocus() const override { return (m_controls & (1u << 17)) != 0; }
    bool hasAbsoluteFocus() const override { return (m_controls & (1u << 5)) != 0; }
    std::optional<bool> autoFocus() override {
        std::uint8_t value = 0;
        if (!request(kGetCur, kFocusAuto, &value, 1)) {
            return std::nullopt;
        }
        return value != 0;
    }
    bool setAutoFocus(bool on) override {
        std::uint8_t value = on ? 1 : 0;
        return request(kSetCur, kFocusAuto, &value, 1);
    }
    std::optional<FocusRange> focusRange() override {
        FocusRange range;
        for (const auto& [code, into] :
             std::array<std::pair<std::uint8_t, int*>, 4>{{{kGetMin, &range.minimum},
                                                           {kGetMax, &range.maximum},
                                                           {kGetRes, &range.resolution},
                                                           {kGetDef, &range.byDefault}}}) {
            const std::optional<int> value = twoBytes(code);
            if (!value) {
                return std::nullopt;
            }
            *into = *value;
        }
        return range;
    }
    std::optional<int> focus() override { return twoBytes(kGetCur); }
    bool setFocus(int position) override {
        std::array<std::uint8_t, 2> bytes{static_cast<std::uint8_t>(position & 0xff),
                                          static_cast<std::uint8_t>((position >> 8) & 0xff)};
        return request(kSetCur, kFocusAbsolute, bytes.data(), 2);
    }

private:
    QString lastError() const {
        return QStringLiteral("0x%1").arg(static_cast<std::uint32_t>(m_lastError), 8, 16);
    }

    std::optional<int> twoBytes(std::uint8_t code) {
        std::array<std::uint8_t, 2> bytes{};
        if (!request(code, kFocusAbsolute, bytes.data(), 2)) {
            return std::nullopt;
        }
        return static_cast<int>(bytes[0] | (bytes[1] << 8));
    }

    bool request(std::uint8_t code, std::uint8_t selector, void* data, std::uint16_t length) {
        IOUSBDevRequest request{};
        // Class requests to an interface: 0xA1 reads, 0x21 writes (UVC 1.1, 4.2).
        request.bmRequestType = (code & 0x80) != 0 ? 0xA1 : 0x21;
        request.bRequest = code;
        request.wValue = static_cast<UInt16>(selector << 8);
        request.wIndex = static_cast<UInt16>((m_terminal << 8) | m_interface);
        request.wLength = length;
        request.pData = data;
        m_lastError = (*m_device)->DeviceRequest(m_device, &request);
        return m_lastError == kIOReturnSuccess;
    }

    IOUSBDeviceInterface** m_device;
    std::uint8_t m_interface;
    std::uint8_t m_terminal;
    std::uint32_t m_controls;
    IOReturn m_lastError = kIOReturnSuccess;
};

} // namespace

std::unique_ptr<Camera> Camera::forQtId(const QByteArray& qtId, QString& why) {
    bool parsed = false;
    const qulonglong packed = qtId.startsWith("0x") ? qtId.mid(2).toULongLong(&parsed, 16) : 0;
    if (!parsed) {
        why = QStringLiteral("not a USB camera's id");
        return nullptr;
    }
    const auto location = static_cast<std::uint32_t>(packed >> 32);
    const auto vendor = static_cast<std::uint32_t>((packed >> 16) & 0xffff);
    const auto product = static_cast<std::uint32_t>(packed & 0xffff);

    io_iterator_t devices = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOUSBHostDevice"), &devices) !=
        KERN_SUCCESS) {
        why = QStringLiteral("no USB devices listed");
        return nullptr;
    }
    std::unique_ptr<Camera> found;
    why = QStringLiteral("no USB device at location 0x%1").arg(location, 8, 16, QLatin1Char('0'));
    while (io_service_t service = IOIteratorNext(devices)) {
        if (numberProperty(service, "locationID") == location &&
            numberProperty(service, "idVendor") == vendor &&
            numberProperty(service, "idProduct") == product && !found) {
            IOCFPlugInInterface** plugIn = nullptr;
            SInt32 score = 0;
            IOUSBDeviceInterface** device = nullptr;
            const kern_return_t made = IOCreatePlugInInterfaceForService(
                service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugIn, &score);
            if (made == KERN_SUCCESS && plugIn) {
                (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                          reinterpret_cast<LPVOID*>(&device));
                IODestroyPlugInInterface(plugIn);
            }
            if (!device) {
                why = QStringLiteral("the USB device could not be opened for requests (0x%1)")
                          .arg(static_cast<std::uint32_t>(made), 8, 16, QLatin1Char('0'));
            } else {
                IOUSBConfigurationDescriptorPtr config = nullptr;
                std::optional<std::uint8_t> interfaceNumber;
                std::optional<std::uint8_t> terminal;
                std::uint32_t controls = 0;
                if ((*device)->GetConfigurationDescriptorPtr(device, 0, &config) == kIOReturnSuccess &&
                    config) {
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(config);
                    const int total = USBToHostWord(config->wTotalLength);
                    bool inVideoControl = false;
                    std::uint8_t currentInterface = 0;
                    for (int at = 0; at + 2 <= total && bytes[at] >= 2; at += bytes[at]) {
                        const std::uint8_t* d = bytes + at;
                        if (d[1] == 0x04 && d[0] >= 9) { // an interface: video (0x0E), control (0x01)?
                            currentInterface = d[2];
                            inVideoControl = d[5] == 0x0E && d[6] == 0x01;
                        } else if (inVideoControl && d[1] == 0x24 && d[2] == 0x02 && d[0] >= 15 &&
                                   (d[4] | (d[5] << 8)) == 0x0201 && !terminal) { // the camera terminal
                            interfaceNumber = currentInterface;
                            terminal = d[3];
                            for (int i = 0; i < d[14] && i < 4 && 15 + i < d[0]; ++i) {
                                controls |= static_cast<std::uint32_t>(d[15 + i]) << (8 * i);
                            }
                        }
                    }
                }
                if (terminal && interfaceNumber) {
                    found = std::make_unique<MacCamera>(device, *interfaceNumber, *terminal, controls);
                } else {
                    why = QStringLiteral("no UVC camera terminal in its descriptors");
                    (*device)->Release(device);
                }
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(devices);
    return found;
}

#elif defined(_WIN32)

namespace {

// DirectShow's camera control: the driver's Focus property, which reaches UVC cameras and most others.
class WindowsCamera final : public Camera {
public:
    explicit WindowsCamera(IAMCameraControl* control) : m_control(control) {
        m_hasRange =
            SUCCEEDED(m_control->GetRange(CameraControl_Focus, &m_min, &m_max, &m_step, &m_default, &m_caps));
    }
    ~WindowsCamera() override { m_control->Release(); }
    WindowsCamera(const WindowsCamera&) = delete;
    WindowsCamera& operator=(const WindowsCamera&) = delete;

    QString summary() override {
        if (!m_hasRange) {
            return QStringLiteral("    DirectShow focus: not offered");
        }
        long value = 0;
        long flags = 0;
        const bool read = SUCCEEDED(m_control->Get(CameraControl_Focus, &value, &flags));
        return QStringLiteral("    DirectShow focus: %1 to %2 by %3, default %4, auto %5, manual %6%7")
            .arg(m_min)
            .arg(m_max)
            .arg(m_step)
            .arg(m_default)
            .arg((m_caps & CameraControl_Flags_Auto) != 0 ? QStringLiteral("offered")
                                                          : QStringLiteral("not offered"))
            .arg((m_caps & CameraControl_Flags_Manual) != 0 ? QStringLiteral("offered")
                                                            : QStringLiteral("not offered"))
            .arg(read ? QStringLiteral("; now %1, auto %2")
                            .arg(value)
                            .arg((flags & CameraControl_Flags_Auto) != 0 ? QStringLiteral("on")
                                                                         : QStringLiteral("off"))
                      : QStringLiteral("; unreadable"));
    }
    bool hasAutoFocus() const override { return m_hasRange && (m_caps & CameraControl_Flags_Auto) != 0; }
    bool hasAbsoluteFocus() const override {
        return m_hasRange && (m_caps & CameraControl_Flags_Manual) != 0;
    }
    std::optional<bool> autoFocus() override {
        long value = 0;
        long flags = 0;
        if (FAILED(m_control->Get(CameraControl_Focus, &value, &flags))) {
            return std::nullopt;
        }
        return (flags & CameraControl_Flags_Auto) != 0;
    }
    bool setAutoFocus(bool on) override {
        long value = m_default;
        long flags = 0;
        m_control->Get(CameraControl_Focus, &value, &flags);
        return SUCCEEDED(m_control->Set(CameraControl_Focus, value,
                                        on ? CameraControl_Flags_Auto : CameraControl_Flags_Manual));
    }
    std::optional<FocusRange> focusRange() override {
        if (!m_hasRange) {
            return std::nullopt;
        }
        return FocusRange{static_cast<int>(m_min), static_cast<int>(m_max), static_cast<int>(m_step),
                          static_cast<int>(m_default)};
    }
    std::optional<int> focus() override {
        long value = 0;
        long flags = 0;
        if (FAILED(m_control->Get(CameraControl_Focus, &value, &flags))) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    }
    bool setFocus(int position) override {
        return SUCCEEDED(m_control->Set(CameraControl_Focus, position, CameraControl_Flags_Manual));
    }

private:
    IAMCameraControl* m_control;
    bool m_hasRange = false;
    long m_min = 0;
    long m_max = 0;
    long m_step = 0;
    long m_default = 0;
    long m_caps = 0;
};

// A device path without its interface class: Qt names a camera by its Media Foundation link and DirectShow by
// its own, and the two differ only after the last "#{".
QString stem(QString path) {
    path = path.toLower();
    const qsizetype at = path.lastIndexOf(QStringLiteral("#{"));
    return at > 0 ? path.left(at) : path;
}

} // namespace

std::unique_ptr<Camera> Camera::forQtId(const QByteArray& qtId, QString& why) {
    // Qt has set COM up on this thread already (S_FALSE, or RPC_E_CHANGED_MODE); it is never ended here, as
    // the camera found keeps COM objects.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<Camera> found;
    why = QStringLiteral("no DirectShow camera with this device path");
    ICreateDevEnum* devices = nullptr;
    IEnumMoniker* monikers = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&devices))) &&
        devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK) {
        const QString wanted = stem(QString::fromUtf8(qtId));
        IMoniker* moniker = nullptr;
        while (!found && monikers->Next(1, &moniker, nullptr) == S_OK) {
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
                        found = std::make_unique<WindowsCamera>(control);
                    } else {
                        why = QStringLiteral("the camera offers no DirectShow camera control");
                    }
                    if (filter) {
                        filter->Release();
                    }
                }
                VariantClear(&path);
                bag->Release();
            }
            moniker->Release();
        }
    }
    if (monikers) {
        monikers->Release();
    }
    if (devices) {
        devices->Release();
    }
    return found;
}

#else

std::unique_ptr<Camera> Camera::forQtId(const QByteArray&, QString& why) {
    why = QStringLiteral("camera controls are reached on macOS and Windows only");
    return nullptr;
}

#endif

} // namespace uvc
