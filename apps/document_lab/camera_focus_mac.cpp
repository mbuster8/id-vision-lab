#include "camera_focus.h"

#include "uvc_descriptor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace document_lab {

namespace {

// UVC 1.1, A.8 (request codes) and A.9.4 (camera terminal control selectors).
constexpr std::uint8_t kSetCur = 0x01;
constexpr std::uint8_t kGetCur = 0x81;
constexpr std::uint8_t kGetMin = 0x82;
constexpr std::uint8_t kGetMax = 0x83;
constexpr std::uint8_t kGetRes = 0x84;
constexpr std::uint8_t kFocusAbsoluteControl = 0x06;
constexpr std::uint8_t kFocusAutoControl = 0x08;
// A camera that stops answering must not hold the processing thread: these bound every request.
constexpr UInt32 kNoDataTimeoutMs = 100;
constexpr UInt32 kCompletionTimeoutMs = 500;

// Qt's id for a USB camera on macOS: "0x" and, in hex, the location (32 bits), vendor (16) and product (16).
struct UsbId {
    std::uint32_t location = 0;
    std::uint32_t vendor = 0;
    std::uint32_t product = 0;
};

std::optional<UsbId> usbIdOf(const QByteArray& qtId) {
    if (!qtId.startsWith("0x") || qtId.size() < 3 || qtId.size() > 18) {
        return std::nullopt;
    }
    bool parsed = false;
    const qulonglong packed = qtId.mid(2).toULongLong(&parsed, 16);
    if (!parsed) {
        return std::nullopt;
    }
    return UsbId{.location = static_cast<std::uint32_t>(packed >> 32),
                 .vendor = static_cast<std::uint32_t>((packed >> 16) & 0xFFFF),
                 .product = static_cast<std::uint32_t>(packed & 0xFFFF)};
}

std::optional<std::uint32_t> numberProperty(io_service_t service, const char* name) {
    const CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    if (key == nullptr) {
        return std::nullopt;
    }
    const CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    CFRelease(key);
    if (value == nullptr) {
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

// The answers after which a camera counts as gone for the rest of the run.
bool gone(IOReturn result) {
    return result == kIOUSBTransactionTimeout || result == kIOReturnTimeout || result == kIOReturnAborted ||
           result == kIOReturnNotResponding || result == kIOReturnNoDevice;
}

// A UVC camera's Focus, Auto and Focus (Absolute), through class requests to its camera terminal. Opened on
// its first call; IOKit's user client takes these requests from any thread, while Qt streams and after it
// stops.
class MacFocusControl final : public lab_core::FocusControl {
public:
    explicit MacFocusControl(UsbId id) : m_id(id) {}
    ~MacFocusControl() override {
        if (m_device != nullptr) {
            (*m_device)->Release(m_device);
        }
    }
    MacFocusControl(const MacFocusControl&) = delete;
    MacFocusControl& operator=(const MacFocusControl&) = delete;

    std::optional<lab_core::FocusRange> range() noexcept override {
        if (!open() || (m_terminal.controls & kUvcFocusAbsolute) == 0) {
            return std::nullopt;
        }
        const std::optional<int> minimum = twoBytes(kGetMin);
        const std::optional<int> maximum = twoBytes(kGetMax);
        const std::optional<int> resolution = twoBytes(kGetRes);
        if (!minimum || !maximum || !resolution || *maximum <= *minimum || *resolution < 1) {
            return std::nullopt;
        }
        return lab_core::FocusRange{.minimum = *minimum, .maximum = *maximum, .step = *resolution};
    }
    bool hasAutoFocus() noexcept override { return open() && (m_terminal.controls & kUvcFocusAuto) != 0; }
    std::optional<bool> autoFocus() noexcept override {
        std::uint8_t value = 0;
        if (!hasAutoFocus() || !request(kGetCur, kFocusAutoControl, &value, 1)) {
            return std::nullopt;
        }
        return value != 0;
    }
    bool setAutoFocus(bool on) noexcept override {
        std::uint8_t value = on ? 1 : 0;
        return hasAutoFocus() && request(kSetCur, kFocusAutoControl, &value, 1);
    }
    std::optional<int> position() noexcept override { return open() ? twoBytes(kGetCur) : std::nullopt; }
    bool setPosition(int position) noexcept override {
        if (!open() || position < 0 || position > 0xFFFF) {
            return false;
        }
        std::array<std::uint8_t, 2> bytes{static_cast<std::uint8_t>(position & 0xFF),
                                          static_cast<std::uint8_t>((position >> 8) & 0xFF)};
        return request(kSetCur, kFocusAbsoluteControl, bytes.data(), 2);
    }

private:
    bool open() noexcept {
        if (m_gone) {
            return false;
        }
        if (m_device != nullptr) {
            return true;
        }
        if (m_tried) {
            return false;
        }
        m_tried = true;
        io_iterator_t devices = 0;
        if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOUSBHostDevice"), &devices) !=
            KERN_SUCCESS) {
            return false;
        }
        while (const io_service_t service = IOIteratorNext(devices)) {
            if (m_device == nullptr && numberProperty(service, "locationID") == m_id.location &&
                numberProperty(service, "idVendor") == m_id.vendor &&
                numberProperty(service, "idProduct") == m_id.product) {
                openDevice(service);
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(devices);
        return m_device != nullptr;
    }

    void openDevice(io_service_t service) noexcept {
        IOCFPlugInInterface** plugIn = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
                                              &plugIn, &score) != KERN_SUCCESS ||
            plugIn == nullptr) {
            return;
        }
        IOUSBDeviceInterface182** device = nullptr;
        (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182),
                                  reinterpret_cast<LPVOID*>(&device));
        IODestroyPlugInInterface(plugIn);
        if (device == nullptr) {
            return;
        }
        IOUSBConfigurationDescriptorPtr configuration = nullptr;
        std::optional<UvcCameraTerminal> terminal;
        if ((*device)->GetConfigurationDescriptorPtr(device, 0, &configuration) == kIOReturnSuccess &&
            configuration != nullptr) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(configuration);
            terminal = findCameraTerminal(std::span(bytes, USBToHostWord(configuration->wTotalLength)));
        }
        if (!terminal) {
            (*device)->Release(device);
            return;
        }
        m_device = device;
        m_terminal = *terminal;
    }

    std::optional<int> twoBytes(std::uint8_t code) noexcept {
        std::array<std::uint8_t, 2> bytes{};
        if (!request(code, kFocusAbsoluteControl, bytes.data(), 2)) {
            return std::nullopt;
        }
        return static_cast<int>(bytes[0] | (bytes[1] << 8));
    }

    bool request(std::uint8_t code, std::uint8_t selector, void* data, std::uint16_t length) noexcept {
        if (m_gone || m_device == nullptr) {
            return false;
        }
        IOUSBDevRequestTO request{};
        // Class requests to an interface: 0xA1 reads, 0x21 writes (UVC 1.1, 4.2).
        request.bmRequestType = (code & 0x80) != 0 ? 0xA1 : 0x21;
        request.bRequest = code;
        request.wValue = static_cast<UInt16>(selector << 8);
        request.wIndex = static_cast<UInt16>((m_terminal.terminalId << 8) | m_terminal.interfaceNumber);
        request.wLength = length;
        request.pData = data;
        request.noDataTimeout = kNoDataTimeoutMs;
        request.completionTimeout = kCompletionTimeoutMs;
        const IOReturn result = (*m_device)->DeviceRequestTO(m_device, &request);
        if (gone(result)) {
            m_gone = true;
        }
        return result == kIOReturnSuccess;
    }

    UsbId m_id;
    bool m_tried = false;
    bool m_gone = false;
    IOUSBDeviceInterface182** m_device = nullptr;
    UvcCameraTerminal m_terminal{};
};

} // namespace

std::unique_ptr<lab_core::FocusControl> makeFocusControl(const QByteArray& qtCameraId) {
    const std::optional<UsbId> id = usbIdOf(qtCameraId);
    if (!id) {
        return nullptr;
    }
    return std::make_unique<MacFocusControl>(*id);
}

} // namespace document_lab
