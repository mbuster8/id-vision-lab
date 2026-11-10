#pragma once

#include "visual_debug/image_description.h"

#include <opencv2/core/mat.hpp>

#include <string_view>

namespace visual_debug {

enum class SendResult {
    Delivered,         // the viewer accepted the image
    ViewerUnavailable, // no viewer is listening, or a retry back-off is still running
    Rejected,          // the viewer replied with an error, for example because it is busy
    InvalidImage,      // empty, not 2-D, too large, or of an unsupported depth or channel count
    Timeout,           // the image went out but no reply came back in time
    InvalidEndpoint,   // the endpoint string cannot be used
};

std::string_view toString(SendResult result);

// What a ping found at the endpoint.
enum class ViewerState {
    Listening,       // a viewer answered
    Paused,          // a viewer answered that it is paused: it takes no images until resumed
    PredatesPing,    // a viewer answered that it does not know a ping: an older one, which still takes images
    Unavailable,     // no answer, an answer from something that is not a viewer, or a retry back-off running
    InvalidEndpoint, // the endpoint string cannot be used
};

// "listening", "paused", "predates ping", "unavailable", "invalid endpoint".
std::string_view toString(ViewerState state);

// What host applications depend on, so their own tests can substitute a fake, or a different transport
// can be added later, without touching their code.
class ImageSender {
public:
    virtual ~ImageSender() = default;
    virtual SendResult send(const cv::Mat& image, std::string_view name) = 0;
    // One frame of a live channel: the viewer shows it in place of the channel's previous frame instead of
    // adding it to the history. By default it is sent as an ordinary image, so existing implementations
    // (a host application's own fake, say) keep working.
    virtual SendResult sendLive(const cv::Mat& frame, std::string_view channel) {
        return send(frame, channel);
    }
    // The same with a description of the image (its value range, transfer, primaries). By default the
    // description is dropped, for the same reason.
    virtual SendResult send(const cv::Mat& image, std::string_view name,
                            const ImageDescription& /*description*/) {
        return send(image, name);
    }
    virtual SendResult sendLive(const cv::Mat& frame, std::string_view channel,
                                const ImageDescription& /*description*/) {
        return sendLive(frame, channel);
    }
    // Whether a viewer answers. By default Listening, so an implementation that does not override it behaves
    // as if one always does.
    virtual ViewerState ping() { return ViewerState::Listening; }
    // Tells the viewer the live channel is finished with: it closes it. By default nothing is sent.
    virtual SendResult endLive(std::string_view /*channel*/) { return SendResult::Delivered; }
};

} // namespace visual_debug
