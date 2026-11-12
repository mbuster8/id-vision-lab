#include "lab_core/document_processor.h"
#include "lab_core/mrz_worker.h"
#include "lab_core/processors.h"

#include "fake_mrz_reader.h"
#include "procedural_page.h"
#include "scripted_finder.h"

#include <doc_vision/guidance.h>
#include <doc_vision/quality_gates.h>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// A4: the Document processor reads the MRZ (spec §4.2), with a scripted reader in Manual mode: each frame's
// job is read right after process() returns, and collected by the next process().

namespace {

using doc_vision::Hint;
using lab_core::DocumentProcessor;
using lab_core::Finding;
using lab_core::MrzWorker;
using lab_core::ProcessorResult;
using test_support::frameAt;
using test_support::goodFrame;
using test_support::nothing;
using test_support::page;
using test_support::ReaderScript;
using test_support::readingOf;
using test_support::Script;
using test_support::scripted;
using test_support::scriptedReader;

const std::string& kLine1 = test_support::kSpecimenLine1;
const std::string& kLine2 = test_support::kSpecimenLine2;

struct Reading {
    std::shared_ptr<Script> finder = std::make_shared<Script>();
    std::shared_ptr<ReaderScript> reader = std::make_shared<ReaderScript>();
    std::unique_ptr<DocumentProcessor> processor;

    Reading() {
        finder->steps = {page(test_support::kPageQuad)};
        reader->last = readingOf(kLine1, kLine2);
        processor = std::make_unique<DocumentProcessor>(
            scripted(finder), doc_vision::Thresholds{},
            DocumentProcessor::Reader{.make = scriptedReader(reader), .mode = MrzWorker::Mode::Manual});
    }

    // Processes the frame, then reads its job, as the worker would before the next frame.
    ProcessorResult step(const frame_io::Frame& frame) {
        ProcessorResult result = processor->process(frame);
        processor->pumpReaderForTests();
        return result;
    }
};

const Finding* findingNamed(const ProcessorResult& result, const std::string& name) {
    for (const Finding& finding : result.findings) {
        if (finding.name == name) {
            return &finding;
        }
    }
    return nullptr;
}

std::vector<std::string> namesOf(const ProcessorResult& result) {
    std::vector<std::string> names;
    for (const Finding& finding : result.findings) {
        names.push_back(finding.name);
    }
    return names;
}

cv::Mat blurred(double sigma) {
    cv::Mat out;
    cv::GaussianBlur(goodFrame(), out, cv::Size(0, 0), sigma);
    return out;
}

double sharpnessOf(const cv::Mat& frame) {
    return doc_vision::measure(frame, test_support::kPageQuad).sharpness;
}

TEST(DocumentProcessorMrz, NoReaderNoReading) {
    auto finder = std::make_shared<Script>();
    finder->steps = {page(test_support::kPageQuad)};
    DocumentProcessor processor(scripted(finder));
    const ProcessorResult result = processor.process(frameAt(goodFrame(), 0));
    ASSERT_TRUE(result.guidance);
    EXPECT_FALSE(result.guidance->reading);
    EXPECT_FALSE(result.guidance->scan);
    EXPECT_FALSE(findingNamed(result, "MRZ reads"));
    EXPECT_FALSE(findingNamed(result, "Reader"));
}

TEST(DocumentProcessorMrz, ReadsOnlyGatePassingFreshPages) {
    const cv::Mat soft = blurred(6.0);
    ASSERT_LT(sharpnessOf(soft), doc_vision::Thresholds{}.sharpness.failsAt);

    Reading blurredPage;
    blurredPage.step(frameAt(soft, 0));
    EXPECT_EQ(blurredPage.reader->reads, 0);

    Reading noPage;
    noPage.finder->steps = {nothing()};
    noPage.step(frameAt(goodFrame(), 0));
    EXPECT_EQ(noPage.reader->reads, 0);

    Reading held;
    held.finder->steps = {page(test_support::kPageQuad), nothing()};
    held.step(frameAt(goodFrame(), 0));
    EXPECT_EQ(held.reader->reads, 1);
    ASSERT_EQ(held.reader->pages.size(), 1u);
    EXPECT_EQ(held.reader->pages[0], cv::Size(1250, 880));
    const ProcessorResult kept = held.step(frameAt(goodFrame(), 1)); // within PageHold's third of a second
    ASSERT_TRUE(kept.guidance);
    EXPECT_TRUE(kept.guidance->outlineOverlay);
    EXPECT_EQ(held.reader->reads, 1);
}

// Reads are collected at the start of the next frame: the third read lands on the fourth result.
TEST(DocumentProcessorMrz, AcceptsOnTheResultAfterTheThirdRead) {
    Reading lab;
    for (int n = 0; n < 3; ++n) {
        const ProcessorResult result = lab.step(frameAt(goodFrame(), n));
        ASSERT_TRUE(result.guidance && result.guidance->reading) << n;
        EXPECT_EQ(result.guidance->reading->agreeing, n) << n;
        EXPECT_FALSE(result.guidance->scan) << n;
    }
    const ProcessorResult accepted = lab.step(frameAt(goodFrame(), 3));
    ASSERT_TRUE(accepted.guidance);
    ASSERT_TRUE(accepted.guidance->scan);
    EXPECT_EQ(accepted.guidance->scan->mrz.surname, "ERIKSSON");
    EXPECT_EQ(accepted.guidance->scan->mrz.text(), kLine1 + "\n" + kLine2);
    EXPECT_EQ(accepted.guidance->scan->reads, 3);
    EXPECT_FALSE(accepted.guidance->reading);
}

// The plan review's critical finding: frames of replaced jobs are freed.
TEST(DocumentProcessorMrz, ReplacedJobsFreeTheirFrames) {
    Reading lab;
    std::vector<cv::Mat> frames;
    for (int n = 0; n < 20; ++n) {
        frames.push_back(goodFrame());
        lab.processor->process(frameAt(frames.back(), n)); // no pump: each job replaces the one waiting
    }
    for (int n = 0; n < 19; ++n) {
        EXPECT_EQ(frames[static_cast<std::size_t>(n)].u->refcount, 1) << n;
    }
}

TEST(DocumentProcessorMrz, ResetKeepsTheReaderOpen) {
    Reading lab;
    lab.step(frameAt(goodFrame(), 0));
    lab.processor->reset();
    lab.step(frameAt(goodFrame(), 1));
    EXPECT_EQ(lab.reader->makes, 1);
    EXPECT_EQ(lab.reader->reads, 2);
}

TEST(DocumentProcessorMrz, TwoReadsOfOneFrameNeverAgree) {
    Reading lab;
    const cv::Mat frame = goodFrame();
    for (int i = 0; i < 5; ++i) {
        lab.step(frameAt(frame, 7)); // the same frame, processed again (a paused replay's step)
    }
    const ProcessorResult result = lab.step(frameAt(frame, 7));
    ASSERT_TRUE(result.guidance && result.guidance->reading);
    EXPECT_EQ(result.guidance->reading->agreeing, 1);
    EXPECT_FALSE(result.guidance->scan);
}

TEST(DocumentProcessorMrz, CapturesTheSharpestAgreeingFrame) {
    const cv::Mat softer = blurred(0.9);
    const cv::Mat sharp = goodFrame();
    const cv::Mat soft = blurred(0.7);
    const double passes = doc_vision::Thresholds{}.sharpness.passesAgainAt;
    ASSERT_GT(sharpnessOf(softer), passes);
    ASSERT_GT(sharpnessOf(soft), sharpnessOf(softer));
    ASSERT_GT(sharpnessOf(sharp), sharpnessOf(soft));
    Reading lab;
    lab.step(frameAt(softer, 0));
    lab.step(frameAt(sharp, 1));
    lab.step(frameAt(soft, 2));
    const ProcessorResult accepted = lab.step(frameAt(goodFrame(), 3));
    ASSERT_TRUE(accepted.guidance && accepted.guidance->scan);
    EXPECT_EQ(accepted.guidance->scan->frame.number, 1u);
    EXPECT_EQ(accepted.guidance->scan->quad, test_support::kPageQuad);
    EXPECT_FALSE(accepted.guidance->scan->gates.empty());
}

TEST(DocumentProcessorMrz, AcceptIsOnEveryResultUntilReset) {
    Reading lab;
    for (int n = 0; n < 3; ++n) {
        lab.step(frameAt(goodFrame(), n));
    }
    const ProcessorResult accepted = lab.step(frameAt(goodFrame(), 3));
    ASSERT_TRUE(accepted.guidance && accepted.guidance->scan);
    const int reads = lab.reader->reads;
    for (int n = 4; n < 9; ++n) {
        const ProcessorResult later = lab.step(frameAt(goodFrame(), n));
        ASSERT_TRUE(later.guidance) << n;
        EXPECT_EQ(later.guidance->scan, accepted.guidance->scan) << n;
    }
    EXPECT_EQ(lab.reader->reads, reads); // nothing read after the accept
    lab.processor->reset();
    const ProcessorResult after = lab.step(frameAt(goodFrame(), 9));
    ASSERT_TRUE(after.guidance);
    EXPECT_FALSE(after.guidance->scan);
    ASSERT_TRUE(after.guidance->reading);
    EXPECT_EQ(after.guidance->reading->agreeing, 0);
}

TEST(DocumentProcessorMrz, OlderResetsReadIsDropped) {
    Reading lab;
    lab.processor->process(frameAt(goodFrame(), 0));
    lab.processor->reset();
    lab.processor->pumpReaderForTests(); // the read of a run reset since
    const ProcessorResult result = lab.processor->process(frameAt(goodFrame(), 1));
    ASSERT_TRUE(result.guidance && result.guidance->reading);
    EXPECT_EQ(result.guidance->reading->agreeing, 0);
}

TEST(DocumentProcessorMrz, OnlyPassportsCount) {
    std::string visa = kLine1;
    visa[0] = 'V';
    const std::vector<doc_vision::MrzReading> others{
        readingOf("I<UTOERIKSSON<<ANNA<MARIA<<<<<<<<<<<", "D231458907UTO7408122F1204159<<<<<<<6"), // TD2
        readingOf(visa, kLine2)};
    for (const doc_vision::MrzReading& other : others) {
        Reading lab;
        lab.reader->last = other;
        for (int n = 0; n < 5; ++n) {
            lab.step(frameAt(goodFrame(), n));
        }
        const ProcessorResult result = lab.step(frameAt(goodFrame(), 5));
        ASSERT_TRUE(result.guidance && result.guidance->reading);
        EXPECT_EQ(result.guidance->reading->agreeing, 0);
        EXPECT_FALSE(result.guidance->scan);
    }
}

TEST(DocumentProcessorMrz, FindingsBeforeAndAfterTheAccept) {
    Reading lab;
    const ProcessorResult first = lab.step(frameAt(goodFrame(), 0));
    EXPECT_EQ(namesOf(first),
              (std::vector<std::string>{"Document", "Fill", "Tilt", "Sharpness", "Glare", "Exposure", "Gates",
                                        "Detector", "MRZ reads", "Reader"}));
    EXPECT_EQ(*findingNamed(first, "MRZ reads"),
              (Finding{"MRZ reads", "0 of 3 agree", Finding::Tone::Plain}));
    EXPECT_EQ(*findingNamed(first, "Reader"), (Finding{"Reader", "MRZScanner · –", Finding::Tone::Plain}));
    const ProcessorResult second = lab.step(frameAt(goodFrame(), 1));
    EXPECT_EQ(*findingNamed(second, "MRZ reads"),
              (Finding{"MRZ reads", "1 of 3 agree", Finding::Tone::Plain}));
    EXPECT_EQ(*findingNamed(second, "Reader"),
              (Finding{"Reader", "MRZScanner · 160.0 ms", Finding::Tone::Plain}));
    lab.step(frameAt(goodFrame(), 2));
    const ProcessorResult accepted = lab.step(frameAt(goodFrame(), 3));
    EXPECT_EQ(accepted.findings, (std::vector<Finding>{{"MRZ", "accepted, 3 of 3", Finding::Tone::Good},
                                                       {"Format", "TD3 passport", Finding::Tone::Plain},
                                                       {"Check digits", "5 of 5 pass", Finding::Tone::Plain},
                                                       {"Time to scan", "0.1 s", Finding::Tone::Plain},
                                                       {"Captured", "frame 3", Finding::Tone::Plain}}));
}

TEST(DocumentProcessorMrz, ReaderUnavailableKeepsGuiding) {
    Reading lab;
    lab.reader->failMake = true;
    ProcessorResult result;
    for (int n = 0; n < 30; ++n) {
        result = lab.step(frameAt(goodFrame(), n));
    }
    ASSERT_TRUE(result.guidance && result.guidance->reading);
    EXPECT_FALSE(result.guidance->reading->available);
    EXPECT_EQ(result.guidance->reading->reason, "model missing");
    EXPECT_EQ(result.guidance->shownHint, Hint::Good);
    EXPECT_EQ(*findingNamed(result, "Reader"), (Finding{"Reader", "unavailable", Finding::Tone::Bad}));
    EXPECT_EQ(lab.reader->makes, 1);
}

// The lab's own: MRZScanner beside LC100 in the model folder.
TEST(DocumentProcessorMrz, MakeProcessorReads) {
    const std::filesystem::path model = ID_VISION_LAB_LC100_MODEL;
    const auto processor =
        lab_core::makeProcessor(lab_core::ProcessorKind::Document, {.modelFolder = model.parent_path()});
    const ProcessorResult result = processor->process(frameAt(goodFrame(), 0));
    ASSERT_TRUE(result.guidance);
    ASSERT_TRUE(result.guidance->reading);
    EXPECT_TRUE(result.guidance->reading->available);
}

TEST(DocumentProcessorMrz, ReaderErrorIsAFixedRow) {
    Reading lab;
    lab.reader->error = "x";
    lab.step(frameAt(goodFrame(), 0));
    const ProcessorResult result = lab.step(frameAt(goodFrame(), 1));
    EXPECT_EQ(*findingNamed(result, "Reader"), (Finding{"Reader", "error: x", Finding::Tone::Bad}));
    ASSERT_TRUE(result.guidance);
    EXPECT_EQ(result.guidance->frameHint, Hint::Good); // an error is not "no band"
}

// --- Hints 11 and 12, the read window, the turn and the band (spec §3.2, §4.2)

const doc_vision::Quad kQuad = test_support::kPageQuad;

// The specimen read with its band between `top` and `bottom` on the 1250 × 880 page.
doc_vision::MrzReading bandAt(double top, double bottom) {
    doc_vision::MrzReading reading = readingOf(kLine1, kLine2);
    reading.band = doc_vision::Quad{{{60, top}, {1190, top}, {1190, bottom}, {60, bottom}}};
    return reading;
}

std::vector<test_support::Step> steps(std::initializer_list<std::pair<bool, int>> runs) {
    std::vector<test_support::Step> out;
    for (const auto& [found, count] : runs) {
        for (int i = 0; i < count; ++i) {
            out.push_back(found ? page(kQuad) : nothing());
        }
    }
    return out;
}

TEST(DocumentProcessorMrz, NoBandShowsHintEleven) {
    Reading lab;
    lab.reader->last = test_support::noBand();
    const cv::Mat good = goodFrame();
    EXPECT_EQ(lab.step(frameAt(good, 0)).guidance->frameHint, Hint::Good);
    EXPECT_EQ(lab.step(frameAt(good, 1)).guidance->frameHint, Hint::NoMrzBand);
    ProcessorResult later;
    for (int n = 2; n <= 5; ++n) {
        later = lab.step(frameAt(good, n));
    }
    EXPECT_EQ(later.guidance->shownHint, Hint::NoMrzBand); // held 132 ms
    EXPECT_FALSE(later.guidance->bandOverlay);
}

// An ID card's front: no band, ever. After 5 s hint 12 outranks hint 11, so D2's words show.
TEST(DocumentProcessorMrz, HintTwelveAfterFiveSecondsOutranksEleven) {
    Reading lab;
    lab.reader->last = test_support::noBand();
    const cv::Mat good = goodFrame();
    ProcessorResult result;
    for (int n = 0; n <= 151; ++n) {
        result = lab.step(frameAt(good, n));
    }
    EXPECT_EQ(result.guidance->frameHint, Hint::NoMrzBand); // 4983 ms
    result = lab.step(frameAt(good, 152));
    EXPECT_EQ(result.guidance->frameHint, Hint::CannotReadMrz); // 5016 ms
}

TEST(DocumentProcessorMrz, FailingGateKeepsTheReads) {
    Reading lab;
    const cv::Mat good = goodFrame();
    const cv::Mat soft = blurred(6.0);
    lab.step(frameAt(good, 0));
    lab.step(frameAt(good, 1));
    ProcessorResult result;
    for (int n = 2; n < 62; ++n) {
        result = lab.step(frameAt(soft, n)); // two seconds of a blurred page, still found
    }
    ASSERT_TRUE(result.guidance->reading);
    EXPECT_EQ(result.guidance->reading->agreeing, 2);
    lab.step(frameAt(good, 62));
    result = lab.step(frameAt(good, 63));
    EXPECT_TRUE(result.guidance->scan);
}

TEST(DocumentProcessorMrz, WindowClearsAfterASecondWithoutAPage) {
    Reading lab;
    lab.finder->steps = steps({{true, 2}, {false, 31}, {true, 1}});
    const cv::Mat good = goodFrame();
    lab.step(frameAt(good, 0));
    lab.step(frameAt(good, 1));
    ProcessorResult result = lab.step(frameAt(good, 2));
    EXPECT_EQ(result.guidance->reading->agreeing, 2);
    for (int n = 3; n <= 31; ++n) {
        result = lab.step(frameAt(good, n));
    }
    EXPECT_EQ(result.guidance->reading->agreeing, 2); // 990 ms without a page
    result = lab.step(frameAt(good, 32));
    EXPECT_EQ(result.guidance->reading->agreeing, 0); // 1023 ms
    lab.step(frameAt(good, 33));
    result = lab.step(frameAt(good, 34));
    EXPECT_EQ(result.guidance->reading->agreeing, 1);
}

TEST(DocumentProcessorMrz, TimeToScanFromTheFirstPageAfterAClear) {
    Reading lab;
    lab.finder->steps = steps({{true, 1}, {false, 40}, {true, 1}});
    const cv::Mat good = goodFrame();
    ProcessorResult result;
    for (int n = 0; n <= 44; ++n) {
        result = lab.step(frameAt(good, n));
    }
    ASSERT_TRUE(result.guidance->scan);
    EXPECT_EQ(result.guidance->scan->timeToScan, std::chrono::milliseconds(66)); // frame 43 − frame 41
    EXPECT_EQ(result.guidance->scan->reads, 3);
}

TEST(DocumentProcessorMrz, BandFollowsThisFramesQuad) {
    Reading lab;
    doc_vision::Quad shifted = kQuad;
    for (cv::Point2d& corner : shifted) {
        corner.x += 50;
    }
    lab.finder->steps = {page(kQuad), page(kQuad), page(shifted)};
    const cv::Mat good = goodFrame();
    lab.step(frameAt(good, 0));
    const ProcessorResult first = lab.step(frameAt(good, 1));
    ASSERT_TRUE(first.guidance->bandOverlay);
    ASSERT_TRUE(first.guidance->reading && first.guidance->reading->band);
    const auto* polygon = std::get_if<visual_debug::Polygon>(&first.overlays[*first.guidance->bandOverlay]);
    ASSERT_TRUE(polygon);
    EXPECT_EQ(polygon->label, "MRZ band");
    const doc_vision::Quad expected =
        doc_vision::pageToFrame(*readingOf(kLine1, kLine2).band, kQuad, doc_vision::kPageSize);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR((*first.guidance->reading->band)[i].x, expected[i].x, 1e-6);
        EXPECT_NEAR((*first.guidance->reading->band)[i].y, expected[i].y, 1e-6);
    }
    const ProcessorResult moved = lab.step(frameAt(good, 2));
    ASSERT_TRUE(moved.guidance->reading && moved.guidance->reading->band);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR((*moved.guidance->reading->band)[i].x, expected[i].x + 50, 1.0);
        EXPECT_NEAR((*moved.guidance->reading->band)[i].y, expected[i].y, 1.0);
    }
}

TEST(DocumentProcessorMrz, NoBandOnAHeldPage) {
    Reading lab;
    lab.finder->steps = {page(kQuad), page(kQuad), nothing()};
    const cv::Mat good = goodFrame();
    lab.step(frameAt(good, 0));
    ASSERT_TRUE(lab.step(frameAt(good, 1)).guidance->bandOverlay);
    const ProcessorResult held = lab.step(frameAt(good, 2));
    EXPECT_TRUE(held.guidance->outlineOverlay);
    EXPECT_FALSE(held.guidance->bandOverlay);
    EXPECT_FALSE(held.guidance->reading->band);
}

TEST(DocumentProcessorMrz, NoBandWhileTheBannerShowsABadHint) {
    Reading lab;
    const cv::Mat good = goodFrame();
    const cv::Mat soft = blurred(6.0);
    lab.step(frameAt(good, 0));
    for (int n = 1; n <= 20; ++n) {
        lab.step(frameAt(soft, n));
    }
    const ProcessorResult passing = lab.step(frameAt(good, 21)); // one gate-passing frame in a blurred run
    EXPECT_EQ(passing.guidance->shownHint, Hint::Blurred);
    EXPECT_FALSE(passing.guidance->bandOverlay);
}

TEST(DocumentProcessorMrz, TurnedReadTurnsTheNextJob) {
    Reading lab;
    lab.reader->readings = {bandAt(30, 120), bandAt(760, 850)}; // the page as flattened is upside down
    lab.reader->last = bandAt(760, 850);
    lab.finder->steps = steps({{true, 2}, {false, 12}, {true, 1}});
    const cv::Mat good = goodFrame();
    lab.step(frameAt(good, 0)); // read as flattened (band at the top), then turned
    lab.step(frameAt(good, 1)); // read turned first
    EXPECT_EQ(lab.reader->bottomDarker, (std::vector<bool>{true, false, false}));
    for (int n = 2; n <= 13; ++n) {
        lab.step(frameAt(good, n)); // the page lost past PageHold: its corners, and so its turn, forgotten
    }
    lab.step(frameAt(good, 14));
    EXPECT_TRUE(lab.reader->bottomDarker.back());
}

TEST(DocumentProcessorMrz, ScanQuadIsUpright) {
    Reading lab;
    lab.reader->readings = {bandAt(30, 120), bandAt(760, 850)};
    lab.reader->last = bandAt(760, 850);
    const cv::Mat good = goodFrame();
    ProcessorResult result;
    for (int n = 0; n <= 3; ++n) {
        result = lab.step(frameAt(good, n));
    }
    ASSERT_TRUE(result.guidance->scan);
    const doc_vision::Quad& quad = result.guidance->scan->quad;
    EXPECT_EQ(quad[0], kQuad[2]);
    EXPECT_EQ(quad[1], kQuad[3]);
    EXPECT_EQ(quad[2], kQuad[0]);
    EXPECT_EQ(quad[3], kQuad[1]);
    // The band, turned back, lies along the page's bottom as read: next to corners 3 and 4 (the frame's top).
    const doc_vision::Quad& band = result.guidance->scan->band;
    EXPECT_LT((band[0].y + band[1].y + band[2].y + band[3].y) / 4, (kQuad[0].y + kQuad[3].y) / 2);
    // Final review F4: its corner 1 is its top left as read, as the quad's is. The page is upside down in the
    // frame, so going right along the band as read goes left in the frame, and going down goes up.
    EXPECT_GT(band[0].x, band[1].x);
    EXPECT_GT(band[0].y, band[3].y);
}

} // namespace
