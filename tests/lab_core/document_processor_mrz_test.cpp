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

#include <filesystem>
#include <memory>
#include <string>
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

} // namespace
