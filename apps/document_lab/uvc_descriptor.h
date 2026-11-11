#pragma once

// The camera terminal of a USB Video Class camera, read from its configuration descriptor (UVC 1.1, 3.7.2.3):
// where the lab sends its focus requests on macOS (A3.3).

#include <cstdint>
#include <optional>
#include <span>

namespace document_lab {

struct UvcCameraTerminal {
    std::uint8_t interfaceNumber = 0; // the VideoControl interface
    std::uint8_t terminalId = 0;
    std::uint32_t controls = 0; // bmControls, up to three bytes
    bool operator==(const UvcCameraTerminal&) const = default;
};

// bmControls' bits for the focus controls.
inline constexpr std::uint32_t kUvcFocusAbsolute = 1U << 5;
inline constexpr std::uint32_t kUvcFocusAuto = 1U << 17;

// The first camera terminal (VC_INPUT_TERMINAL, 0x02, of type 0x0201) of a VideoControl interface (class
// 0x0E, subclass 0x01) in a configuration descriptor. Nothing on any inconsistency: the bytes come from the
// device, so no byte is read outside a descriptor whose length fits.
std::optional<UvcCameraTerminal> findCameraTerminal(std::span<const std::uint8_t> configuration);

} // namespace document_lab
