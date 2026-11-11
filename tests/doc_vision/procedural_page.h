#pragma once

// A synthetic TD3 data page for doc_vision's tests and the processor's: no real document and no specimen
// text, drawn with OpenCV alone so it needs no font file.

#include "doc_vision/page_geometry.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif

#include <cmath>
#include <string>
#include <vector>

namespace test_support {

// The page's paper: luma about 189 (0.299 · 196 + 0.587 · 188 + 0.114 · 176), under the Light gate's 215.
inline const cv::Scalar kPaper{176, 188, 196};
inline const cv::Scalar kDesk{45, 40, 50};

// 1250 × 880 (doc_vision::kPageSize): paper with a dark border, guilloche lines, a darker photo box, six text
// bars and two MRZ-like rows of Hershey glyphs at the bottom.
inline cv::Mat proceduralPage() {
    cv::Mat page(doc_vision::kPageSize, CV_8UC3, kPaper);
    cv::rectangle(page, cv::Rect(0, 0, page.cols, page.rows), cv::Scalar(30, 50, 80), 6);
    for (int k = 0; k < 12; ++k) {
        std::vector<cv::Point> line;
        for (int x = 400; x <= 1220; x += 4) {
            line.emplace_back(x, 120 + k * 50 + static_cast<int>(18 * std::sin(x / 40.0 + k)));
        }
        cv::polylines(page, line, false, cv::Scalar(150, 160, 172), 1, cv::LINE_AA);
    }
    cv::rectangle(page, cv::Rect(60, 200, 300, 420), cv::Scalar(110, 115, 120), cv::FILLED);
    const int bars[6] = {420, 640, 300, 560, 480, 360};
    for (int i = 0; i < 6; ++i) {
        cv::rectangle(page, cv::Rect(420, 220 + i * 55, bars[i], 14), cv::Scalar(90, 95, 100), cv::FILLED);
    }
    const std::string first = "P<XXXSAMPLE<<PAGE<<<<<<<<<<<<<<<<<<<<<<<<<<<";
    const std::string second = "0<1<2<3<4<5<6<7<8<9<0<1<2<3<4<5<6<7<8<9<0<1<";
    cv::putText(page, first.substr(0, 44), {60, 770}, cv::FONT_HERSHEY_PLAIN, 2.0, cv::Scalar(30, 30, 30), 2,
                cv::LINE_AA);
    cv::putText(page, second.substr(0, 44), {60, 835}, cv::FONT_HERSHEY_PLAIN, 2.0, cv::Scalar(30, 30, 30), 2,
                cv::LINE_AA);
    return page;
}

// `page` warped onto a darker desk of `frame` pixels, its corners at `quad` (TL, TR, BR, BL).
inline cv::Mat pageInFrame(const cv::Mat& page, const doc_vision::Quad& quad, cv::Size frame) {
    const float right = static_cast<float>(page.cols - 1);
    const float bottom = static_cast<float>(page.rows - 1);
    const std::vector<cv::Point2f> from{{0, 0}, {right, 0}, {right, bottom}, {0, bottom}};
    const std::vector<cv::Point2f> to(quad.begin(), quad.end());
    cv::Mat out(frame, CV_8UC3, kDesk);
    cv::warpPerspective(page, out, cv::getPerspectiveTransform(from, to), frame, cv::INTER_LINEAR,
                        cv::BORDER_TRANSPARENT);
    return out;
}

// The quad most tests put the page at, in a 1280 × 720 frame.
inline const doc_vision::Quad kPageQuad{{{220, 110}, {1010, 150}, {990, 640}, {240, 600}}};

// Intersection over union of two convex quads.
inline double quadIoU(const doc_vision::Quad& a, const doc_vision::Quad& b) {
    const std::vector<cv::Point2f> pa(a.begin(), a.end());
    const std::vector<cv::Point2f> pb(b.begin(), b.end());
    std::vector<cv::Point2f> overlap;
    const double inter = cv::intersectConvexConvex(pa, pb, overlap, true);
    const double sum = cv::contourArea(pa) + cv::contourArea(pb) - inter;
    return sum > 0 ? inter / sum : 0.0;
}

} // namespace test_support
