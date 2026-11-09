#include "frame_io/frame.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

namespace {

using visual_debug::ValueRange;

// What PNG can hold and the processors read: 8- or 16-bit, grey, BGR or BGRA.
TEST(Frame, AcceptedImages) {
    EXPECT_TRUE(frame_io::isAcceptedImage(cv::Mat(2, 2, CV_8UC3)));
    EXPECT_TRUE(frame_io::isAcceptedImage(cv::Mat(2, 2, CV_16UC1)));
    EXPECT_TRUE(frame_io::isAcceptedImage(cv::Mat(2, 2, CV_8UC4)));
    EXPECT_FALSE(frame_io::isAcceptedImage(cv::Mat(2, 2, CV_32FC1)));
    EXPECT_FALSE(frame_io::isAcceptedImage(cv::Mat(2, 2, CV_8UC2)));
    EXPECT_FALSE(frame_io::isAcceptedImage(cv::Mat()));
}

// A 16-bit frame without a value range would be stretched by Auto from its own extremes, frame by frame; its
// nominal range keeps its brightness steady.
TEST(Frame, SixteenBitWithoutARangeGetsTheNominalOne) {
    EXPECT_EQ(frame_io::tagsFor(cv::Mat(2, 2, CV_16UC1), {}).valueRange, (ValueRange{0, 65535}));
    EXPECT_EQ(frame_io::tagsFor(cv::Mat(2, 2, CV_16UC3), {.valueRange = {4100, 60218}}).valueRange,
              (ValueRange{4100, 60218}));
    EXPECT_TRUE(frame_io::tagsFor(cv::Mat(2, 2, CV_8UC1), {}).valueRange.empty());
}

} // namespace
