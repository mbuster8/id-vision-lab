#include "visual_debug/image_sender.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace {

using visual_debug::ImageDescription;
using visual_debug::SendResult;

// A host application's fake written before descriptions existed: it overrides only the two-argument send.
class RecordingSender : public visual_debug::ImageSender {
public:
    SendResult send(const cv::Mat&, std::string_view name) override {
        m_names.emplace_back(name);
        return SendResult::Delivered;
    }
    const std::vector<std::string>& names() const { return m_names; }

private:
    std::vector<std::string> m_names;
};

TEST(ImageSender, DescribedSendsFallBackToThePlainOnes) {
    RecordingSender fake;
    visual_debug::ImageSender& sender = fake;
    const cv::Mat image(2, 2, CV_16UC1, cv::Scalar(0));
    const ImageDescription description{.valueRange = {0, 1023}, .transfer = visual_debug::Transfer::Hlg};

    EXPECT_EQ(sender.send(image, "still", description), SendResult::Delivered);
    EXPECT_EQ(sender.sendLive(image, "camera", description), SendResult::Delivered);

    EXPECT_EQ(fake.names(), (std::vector<std::string>{"still", "camera"}));
}

// The same fake answers a ping and an end as if a viewer always did, sending nothing.
TEST(ImageSender, DefaultPingAndEnd) {
    RecordingSender fake;
    visual_debug::ImageSender& sender = fake;

    EXPECT_EQ(sender.ping(), visual_debug::ViewerState::Listening);
    EXPECT_EQ(sender.endLive("x"), SendResult::Delivered);
    EXPECT_TRUE(fake.names().empty());
    EXPECT_EQ(visual_debug::toString(visual_debug::ViewerState::PredatesPing), "predates ping");
    EXPECT_EQ(visual_debug::toString(visual_debug::ViewerState::InvalidEndpoint), "invalid endpoint");
}

} // namespace
