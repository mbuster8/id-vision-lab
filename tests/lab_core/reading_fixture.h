#pragma once

#include "lab_core/document_processor.h"
#include "lab_core/mrz_worker.h"

#include "fake_mrz_reader.h"
#include "procedural_page.h"
#include "scripted_finder.h"

#include <doc_vision/quality_gates.h>

#include <memory>

namespace test_support {

// A Document processor with a scripted finder (the page found on every frame unless the test says otherwise)
// and a scripted reader (ICAO's TD3 specimen unless the test says otherwise), in Manual mode: each frame's
// job is read right after process() returns, and collected by the next process().
struct Reading {
    std::shared_ptr<Script> finder = std::make_shared<Script>();
    std::shared_ptr<ReaderScript> reader = std::make_shared<ReaderScript>();
    std::unique_ptr<lab_core::DocumentProcessor> processor;

    Reading() {
        finder->steps = {page(kPageQuad)};
        reader->last = readingOf(kSpecimenLine1, kSpecimenLine2);
        processor = std::make_unique<lab_core::DocumentProcessor>(
            scripted(finder), doc_vision::Thresholds{},
            lab_core::DocumentProcessor::Reader{.make = scriptedReader(reader),
                                                .mode = lab_core::MrzWorker::Mode::Manual});
    }

    // Processes the frame, then reads its job, as the worker would before the next frame.
    lab_core::ProcessorResult step(const frame_io::Frame& frame) {
        lab_core::ProcessorResult result = processor->process(frame);
        processor->pumpReaderForTests();
        return result;
    }
};

} // namespace test_support
