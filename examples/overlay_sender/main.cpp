// Overlays: an image sent with shapes the viewer draws over it, without touching its pixels. A made-up ID
// card stands for a camera frame, and the overlays for what a document detector found in it. Start the viewer
// first.

#include "visual_debug/send_image.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <vector>

int main() {
    // A light card, slightly turned, on a dark desk, with a photo box, two lines of text and an MRZ band.
    cv::Mat card(1000, 1600, CV_8UC3, cv::Scalar(40, 40, 40));
    const std::vector<cv::Point> corners{{210, 170}, {1390, 140}, {1410, 880}, {190, 900}};
    cv::fillConvexPoly(card, corners, cv::Scalar(225, 225, 225));
    cv::rectangle(card, cv::Rect(300, 280, 300, 380), cv::Scalar(150, 150, 150), cv::FILLED);
    for (const int y : {320, 400}) {
        cv::rectangle(card, cv::Rect(680, y, 560, 30), cv::Scalar(90, 90, 90), cv::FILLED);
    }
    for (const int y : {770, 820}) {
        cv::rectangle(card, cv::Rect(260, y, 1080, 30), cv::Scalar(60, 60, 60), cv::FILLED);
    }

    // Written out qualified: <windows.h> declares Polygon and Rectangle functions of its own.
    const visual_debug::ImageDescription found{
        .overlays = {
            visual_debug::Polygon{visual_debug::toPoints(corners), "Document"},
            visual_debug::Rectangle{{300, 280, 300, 380}, "Photo"},
            visual_debug::Polygon{
                {{250, 755}, {1350, 745}, {1352, 862}, {252, 872}}, "MRZ", visual_debug::kOverlayRed},
            visual_debug::Circle{{450, 430}, 110}, // the face, without a label
            visual_debug::Point{{210, 170}, "corner"},
        }};

    const visual_debug::SendResult result = visual_debug::sendImage(card, "card with overlays", found);
    std::cout << "card with overlays: " << visual_debug::toString(result) << '\n';
    return result == visual_debug::SendResult::Delivered ? 0 : 1;
}
