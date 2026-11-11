#include "uvc_descriptor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using document_lab::findCameraTerminal;
using document_lab::UvcCameraTerminal;

using Bytes = std::vector<std::uint8_t>;

const Bytes kInterface{0x09, 0x04, 0x00, 0x00, 0x01, 0x0E, 0x01, 0x00, 0x00}; // video (0x0E), control (0x01)
const Bytes kHeader{0x0D, 0x24, 0x01, 0x00, 0x01, 0x0D, 0x00, 0x00, 0x6C, 0xDC, 0x02, 0x01, 0x01};
// The C920's camera terminal: id 1, type 0x0201, three bytes of controls 0x020A2E (Focus (Absolute), Focus,
// Auto).
const Bytes kTerminal{0x12, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x2E, 0x0A, 0x02};

// A configuration descriptor holding `parts`, with its total length.
Bytes configuration(const std::vector<Bytes>& parts) {
    Bytes bytes{0x09, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA};
    for (const Bytes& part : parts) {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    bytes[2] = static_cast<std::uint8_t>(bytes.size() & 0xFF);
    bytes[3] = static_cast<std::uint8_t>(bytes.size() >> 8);
    return bytes;
}

TEST(UvcDescriptor, FindsTheC920sCameraTerminal) {
    const Bytes bytes = configuration({kInterface, kHeader, kTerminal});
    const auto terminal = findCameraTerminal(bytes);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(*terminal, (UvcCameraTerminal{.interfaceNumber = 0, .terminalId = 1, .controls = 0x020A2E}));
    EXPECT_NE(terminal->controls & document_lab::kUvcFocusAuto, 0U);
    EXPECT_NE(terminal->controls & document_lab::kUvcFocusAbsolute, 0U);
}

// The last descriptor ends exactly at the end of the bytes: it is read.
TEST(UvcDescriptor, LastDescriptorEndingAtTheEndIsRead) {
    const Bytes bytes = configuration({kInterface, kHeader, kTerminal});
    ASSERT_EQ(bytes.size(), 9U + 9U + 13U + 18U);
    EXPECT_TRUE(findCameraTerminal(bytes));
}

TEST(UvcDescriptor, TruncatedTerminalIsNothing) {
    Bytes bytes = configuration({kInterface, kHeader, kTerminal});
    bytes.resize(bytes.size() - 5);
    EXPECT_FALSE(findCameraTerminal(bytes));
}

TEST(UvcDescriptor, LengthPastTheEndIsNothing) {
    Bytes terminal = kTerminal;
    terminal[0] = 0x40;
    EXPECT_FALSE(findCameraTerminal(configuration({kInterface, kHeader, terminal})));
}

TEST(UvcDescriptor, ZeroOrOneLengthIsNothing) {
    for (const std::uint8_t length : {std::uint8_t{0}, std::uint8_t{1}}) {
        Bytes header = kHeader;
        header[0] = length;
        EXPECT_FALSE(findCameraTerminal(configuration({kInterface, header, kTerminal}))) << int{length};
    }
}

TEST(UvcDescriptor, NoCameraTerminalIsNothing) {
    EXPECT_FALSE(findCameraTerminal(configuration({kInterface, kHeader})));
    EXPECT_FALSE(findCameraTerminal(Bytes{}));
    EXPECT_FALSE(findCameraTerminal(Bytes{0x09}));
}

TEST(UvcDescriptor, TerminalOutsideVideoControlIsIgnored) {
    Bytes audio = kInterface;
    audio[5] = 0x01; // an audio interface
    EXPECT_FALSE(findCameraTerminal(configuration({audio, kHeader, kTerminal})));
}

// A terminal whose control bytes would run past its own length is nothing.
TEST(UvcDescriptor, ControlsPastTheTerminalAreNothing) {
    Bytes terminal = kTerminal;
    terminal[14] = 0x09; // nine bytes of controls in an 18-byte descriptor
    EXPECT_FALSE(findCameraTerminal(configuration({kInterface, kHeader, terminal})));
}

} // namespace
