#include "lab_core/document_processor.h"
#include "lab_core/engine.h"
#include "lab_core/mrz_worker.h"
#include "lab_core/result_sender.h"

#include "fake_mrz_reader.h"
#include "fake_sender.h"
#include "procedural_page.h"
#include "scripted_finder.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// A4, spec §2's anti-goal: the MRZ goes nowhere but Review and a save the user chooses. Not into findings,
// overlay labels, errors or the stream, whatever the reader reads.

namespace {

using lab_core::DocumentProcessor;
using lab_core::MrzWorker;
using lab_core::ProcessorResult;
using test_support::frameAt;
using test_support::goodFrame;
using test_support::page;
using test_support::ReaderScript;
using test_support::readingOf;
using test_support::Script;
using test_support::scripted;
using test_support::scriptedReader;

// The specimen's lines, and the fields a leak would most likely show.
std::vector<std::string> secrets() {
    return {test_support::kSpecimenLine1,
            test_support::kSpecimenLine2,
            "ERIKSSON",
            "ANNA",
            "L898902C3",
            "ZE184226B"};
}

// Whether `text` holds any of them; the one it holds, in `which`.
bool leaks(const std::string& text, std::string& which) {
    for (const std::string& secret : secrets()) {
        if (text.find(secret) != std::string::npos) {
            which = secret;
            return true;
        }
    }
    return false;
}

std::string labelOf(const visual_debug::Overlay& overlay) {
    return std::visit([](const auto& shape) { return shape.label; }, overlay);
}

// Every finding's name and value, every overlay's label, and the processor's error.
void expectNoMrzIn(const ProcessorResult& result, const std::string& where) {
    std::string which;
    for (const lab_core::Finding& finding : result.findings) {
        EXPECT_FALSE(leaks(finding.name + " " + finding.value, which))
            << where << ": finding holds " << which;
    }
    for (const visual_debug::Overlay& overlay : result.overlays) {
        EXPECT_FALSE(leaks(labelOf(overlay), which)) << where << ": overlay label holds " << which;
    }
    for (const lab_core::ProcessedImage& image : result.images) {
        EXPECT_FALSE(leaks(image.name, which)) << where << ": picture name holds " << which;
        for (const visual_debug::Overlay& overlay : image.description.overlays) {
            EXPECT_FALSE(leaks(labelOf(overlay), which)) << where << ": picture's overlay holds " << which;
        }
    }
    if (result.guidance) {
        EXPECT_FALSE(leaks(result.guidance->reason, which)) << where << ": reason holds " << which;
        if (result.guidance->reading) {
            EXPECT_FALSE(leaks(result.guidance->reading->reason, which))
                << where << ": reader reason holds " << which;
        }
    }
}

struct Reading {
    std::shared_ptr<Script> finder = std::make_shared<Script>();
    std::shared_ptr<ReaderScript> reader = std::make_shared<ReaderScript>();
    std::unique_ptr<DocumentProcessor> processor;

    Reading() {
        finder->steps = {page(test_support::kPageQuad)};
        reader->last = readingOf(test_support::kSpecimenLine1, test_support::kSpecimenLine2);
        processor = std::make_unique<DocumentProcessor>(
            scripted(finder), doc_vision::Thresholds{},
            DocumentProcessor::Reader{.make = scriptedReader(reader), .mode = MrzWorker::Mode::Manual});
    }

    ProcessorResult step(int n) {
        ProcessorResult result = processor->process(frameAt(goodFrame(), n));
        processor->pumpReaderForTests();
        return result;
    }
};

TEST(MrzPrivacy, ProcessorKeepsTheMrzOutOfFindingsAndLabels) {
    Reading reading;
    bool accepted = false;
    for (int n = 0; n < 15; ++n) {
        const ProcessorResult result = reading.step(n);
        expectNoMrzIn(result, "frame " + std::to_string(n));
        accepted = accepted || (result.guidance && result.guidance->scan);
    }
    EXPECT_TRUE(
        accepted); // the specimen was read, agreed on and accepted: the rows after it were checked too
}

// Reads that fail (an error, a thrown read, lines that do not check) leave nothing of what was read either.
TEST(MrzPrivacy, ReaderErrorsHoldNoReadText) {
    {
        Reading reading;
        reading.reader->error = "the decoder gave up";
        for (int n = 0; n < 5; ++n) {
            expectNoMrzIn(reading.step(n), "error, frame " + std::to_string(n));
        }
    }
    {
        Reading reading;
        reading.reader->throwOnRead = true;
        for (int n = 0; n < 5; ++n) {
            expectNoMrzIn(reading.step(n), "throw, frame " + std::to_string(n));
        }
    }
    {
        Reading reading;
        std::string wrong = test_support::kSpecimenLine2;
        wrong[9] = wrong[9] == '6' ? '5' : '6'; // the document number's check digit fails
        reading.reader->last = readingOf(test_support::kSpecimenLine1, wrong);
        for (int n = 0; n < 5; ++n) {
            const ProcessorResult result = reading.step(n);
            expectNoMrzIn(result, "invalid, frame " + std::to_string(n));
            for (const lab_core::Finding& finding : result.findings) {
                EXPECT_EQ((finding.name + finding.value).find(wrong), std::string::npos);
            }
        }
    }
}

// The stream sends the processor's overlays and findings with each frame: none of them holds the MRZ.
TEST(MrzPrivacy, StreamGetsNoMrz) {
    auto finder = std::make_shared<Script>();
    finder->steps = {page(test_support::kPageQuad)};
    auto reader = std::make_shared<ReaderScript>();
    reader->last = readingOf(test_support::kSpecimenLine1, test_support::kSpecimenLine2);
    auto sender = std::make_unique<test_support::RecordingSender>();
    test_support::RecordingSender* sent = sender.get();
    lab_core::Engine engine(lab_core::Engine::Mode::Immediate, {});
    engine.setProcessor(std::make_unique<DocumentProcessor>(
        scripted(finder), doc_vision::Thresholds{},
        DocumentProcessor::Reader{.make = scriptedReader(reader), .mode = MrzWorker::Mode::Immediate}));
    auto resultSender =
        std::make_unique<lab_core::ResultSender>(lab_core::ResultSender::Mode::Immediate, std::move(sender));
    lab_core::ResultSender* sending = resultSender.get(); // the engine owns it; the test pumps it
    engine.setSender(std::move(resultSender));
    engine.startLive("Test camera");

    bool accepted = false;
    for (int n = 0; n < 15; ++n) {
        engine.offer(
            {[n](std::string&) -> std::optional<frame_io::Frame> { return frameAt(goodFrame(), n); }});
        engine.pump();
        sending->pump();
        if (const std::optional<lab_core::LabResult> result = engine.takeResult()) {
            accepted = accepted || (result->processed.guidance && result->processed.guidance->scan);
        }
    }

    EXPECT_TRUE(accepted);
    const std::vector<test_support::RecordingSender::Call> calls = sent->calls();
    ASSERT_FALSE(calls.empty());
    std::string which;
    for (const test_support::RecordingSender::Call& call : calls) {
        EXPECT_FALSE(leaks(call.name, which)) << "a channel's name holds " << which;
        for (const visual_debug::Overlay& overlay : call.description.overlays) {
            EXPECT_FALSE(leaks(labelOf(overlay), which)) << "an overlay sent holds " << which;
        }
    }
}

} // namespace
