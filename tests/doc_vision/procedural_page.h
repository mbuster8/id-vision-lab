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

// ICAO Doc 9303's public TD3 specimen MRZ (no real person).
inline const std::string kSpecimenLine1 = "P<UTOERIKSSON<<ANNA<MARIA<<<<<<<<<<<<<<<<<<<";
inline const std::string kSpecimenLine2 = "L898902C36UTO7408122F1204159ZE184226B<<<<<10";

// A flattened 1250 × 880 TD3 page (10 px/mm) with the specimen's MRZ drawn in OpenCV's Hershey SIMPLEX at
// ICAO's 2.54 mm pitch: the page measured on 12 November, on which MRZScanner finds the band (y 765–848, x
// 50–1183) and two lines of 44, with 2–7 characters a line misread.
inline cv::Mat specimenPage() {
    constexpr double kScale = 10.0; // px per mm
    constexpr int kThickness = 2;
    cv::Mat page(880, 1250, CV_8UC3, cv::Scalar(225, 232, 236));
    cv::rectangle(page, cv::Rect(60, 180, 320, 400), cv::Scalar(150, 160, 170), cv::FILLED);
    int baseline = 0;
    const cv::Size cap = cv::getTextSize("H", cv::FONT_HERSHEY_SIMPLEX, 1.0, kThickness, &baseline);
    const double fontScale = 2.4 * kScale / cap.height;
    const auto line = [&](const std::string& text, double baselineMm) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const cv::Point at(static_cast<int>((6.4 + 2.54 * static_cast<double>(i)) * kScale),
                               static_cast<int>(baselineMm * kScale));
            cv::putText(page, std::string(1, text[i]), at, cv::FONT_HERSHEY_SIMPLEX, fontScale,
                        cv::Scalar(20, 20, 20), kThickness, cv::LINE_AA);
        }
    };
    line(kSpecimenLine1, 79.0);
    line(kSpecimenLine2, 84.0);
    return page;
}

} // namespace test_support
