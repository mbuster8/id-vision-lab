#include "camera_focus.h"

#include <gtest/gtest.h>

#include <QByteArray>

#include <cstdio>
#include <memory>
#include <optional>

namespace {

using document_lab::makeFocusControl;

// The ids Qt gave the owner's cameras (spec §12): the C920 on macOS, the Legion's integrated camera on
// Windows.
const QByteArray kMacUsbId = "0x1100000046d082d";
const QByteArray kWindowsUsbPath =
    R"(\\?\usb#vid_04f2&pid_b7b6&mi_00#6&16f8534&1&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\global)";

TEST(CameraFocus, UnknownIdHasNoControl) {
    EXPECT_EQ(makeFocusControl("c"), nullptr);
    EXPECT_EQ(makeFocusControl("9F06D613-0E23-4E48-9735-1F6700000001"), nullptr); // an iPhone
    EXPECT_EQ(makeFocusControl("0xnothex"), nullptr);
    EXPECT_EQ(makeFocusControl(""), nullptr);
}

// Decided from the id alone: no device is opened until the control's first call, on the processing thread.
TEST(CameraFocus, UsbIdHasAControlOnThisPlatform) {
#if defined(__APPLE__)
    EXPECT_NE(makeFocusControl(kMacUsbId), nullptr);
    EXPECT_EQ(makeFocusControl(kWindowsUsbPath), nullptr);
#elif defined(_WIN32)
    EXPECT_NE(makeFocusControl(kWindowsUsbPath), nullptr);
    EXPECT_EQ(makeFocusControl(kMacUsbId), nullptr);
#else
    EXPECT_EQ(makeFocusControl(kMacUsbId), nullptr);
    EXPECT_EQ(makeFocusControl(kWindowsUsbPath), nullptr);
#endif
}

// By hand, with the owner's C920 plugged into the Mac: reads its focus controls through the real control and
// writes nothing. --gtest_also_run_disabled_tests runs it.
TEST(CameraFocus, DISABLED_ReadsTheC920) {
    const std::unique_ptr<lab_core::FocusControl> control = makeFocusControl(kMacUsbId);
    ASSERT_NE(control, nullptr);
    const std::optional<lab_core::FocusRange> range = control->range();
    ASSERT_TRUE(range);
    std::printf("range %d to %d by %d, autofocus offered %d, on %d, position %d\n", range->minimum,
                range->maximum, range->step, control->hasAutoFocus() ? 1 : 0,
                control->autoFocus().value_or(false) ? 1 : 0, control->position().value_or(-1));
    EXPECT_EQ(*range, (lab_core::FocusRange{.minimum = 0, .maximum = 250, .step = 5}));
    EXPECT_TRUE(control->hasAutoFocus());
}

} // namespace
