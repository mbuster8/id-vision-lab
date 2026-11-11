#include "uvc_descriptor.h"

#include <algorithm>
#include <cstddef>

namespace document_lab {

namespace {

constexpr std::uint8_t kInterfaceDescriptor = 0x04;
constexpr std::uint8_t kClassSpecificInterface = 0x24;
constexpr std::uint8_t kVideoClass = 0x0E;
constexpr std::uint8_t kVideoControl = 0x01;
constexpr std::uint8_t kInputTerminal = 0x02;
constexpr std::uint16_t kCameraTerminal = 0x0201;
constexpr std::size_t kInterfaceLength = 9;
constexpr std::size_t kTerminalLength = 15; // up to bControlSize; bmControls follow

} // namespace

std::optional<UvcCameraTerminal> findCameraTerminal(std::span<const std::uint8_t> configuration) {
    bool inVideoControl = false;
    std::uint8_t interfaceNumber = 0;
    std::size_t at = 0;
    while (at < configuration.size()) {
        if (configuration.size() - at < 2) {
            return std::nullopt;
        }
        const std::size_t length = configuration[at];
        if (length < 2 || length > configuration.size() - at) {
            return std::nullopt;
        }
        const std::span<const std::uint8_t> d = configuration.subspan(at, length);
        if (d[1] == kInterfaceDescriptor && length >= kInterfaceLength) {
            interfaceNumber = d[2];
            inVideoControl = d[5] == kVideoClass && d[6] == kVideoControl;
        } else if (inVideoControl && d[1] == kClassSpecificInterface && length >= kTerminalLength &&
                   d[2] == kInputTerminal && (d[4] | (d[5] << 8)) == kCameraTerminal) {
            const std::size_t controlSize = d[14];
            if (kTerminalLength + controlSize > length) {
                return std::nullopt;
            }
            std::uint32_t controls = 0;
            for (std::size_t i = 0; i < std::min<std::size_t>(controlSize, 3); ++i) {
                controls |= static_cast<std::uint32_t>(d[kTerminalLength + i]) << (8 * i);
            }
            return UvcCameraTerminal{
                .interfaceNumber = interfaceNumber, .terminalId = d[3], .controls = controls};
        }
        at += length;
    }
    return std::nullopt;
}

} // namespace document_lab
