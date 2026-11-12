#pragma once

#include "lab_core/mrz_worker.h"

#include <doc_vision/mrz_reader.h>
#include <doc_vision/onnx_model.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

// What a scripted MRZ reader does, shared with the test: the readings it gives in order (the last repeats),
// and what it was asked. It can hold a read, or its own opening, until the test lets go.
struct ReaderScript {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<doc_vision::MrzReading> readings;
    doc_vision::MrzReading last;
    std::string error; // given with every read when set
    bool throwOnRead = false;
    bool blockReads = false; // a read waits until cancelled
    bool blockMake = false;  // opening waits until releaseMake
    bool releaseMake = false;
    bool failMake = false;
    doc_vision::OpenFailure failure = doc_vision::OpenFailure::Model;
    // What happened.
    int makes = 0;
    int reads = 0;
    int cancels = 0;
    bool makeStarted = false;
    bool readStarted = false;
    bool cancelled = false;
    std::vector<cv::Size> pages;
    // Whether each page read was darker at the bottom than at the top: the procedural page's MRZ-like rows
    // are at its bottom, so false means the page was read turned.
    std::vector<bool> bottomDarker;

    // Waits up to five seconds for `ready`; whether it came.
    template <typename Predicate> bool waitFor(Predicate ready) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), ready);
    }
};

class ScriptedReader final : public doc_vision::MrzReader {
public:
    explicit ScriptedReader(std::shared_ptr<ReaderScript> script) : m_script(std::move(script)) {}

    doc_vision::MrzReading read(const cv::Mat& page, std::string& error) override {
        ReaderScript& s = *m_script;
        std::unique_lock lock(s.mutex);
        ++s.reads;
        s.pages.push_back(page.size());
        const int band = page.rows * 15 / 100;
        s.bottomDarker.push_back(cv::mean(page.rowRange(page.rows - band, page.rows))[1] <
                                 cv::mean(page.rowRange(0, band))[1]);
        s.readStarted = true;
        s.changed.notify_all();
        if (s.throwOnRead) {
            throw std::runtime_error("scripted failure");
        }
        if (s.blockReads) {
            s.changed.wait(lock, [&s] { return s.cancelled; });
        }
        if (s.cancelled) {
            error = "cancelled";
            return {};
        }
        error = s.error;
        if (!s.readings.empty()) {
            s.last = s.readings.front();
            s.readings.pop_front();
        }
        return s.last;
    }

    void cancel() override {
        ReaderScript& s = *m_script;
        const std::lock_guard lock(s.mutex);
        ++s.cancels;
        s.cancelled = true;
        s.changed.notify_all();
    }

private:
    std::shared_ptr<ReaderScript> m_script;
};

inline lab_core::MrzWorker::MakeReader scriptedReader(const std::shared_ptr<ReaderScript>& script) {
    return [script](std::string& error,
                    doc_vision::OpenFailure& failure) -> std::unique_ptr<doc_vision::MrzReader> {
        std::unique_lock lock(script->mutex);
        ++script->makes;
        script->makeStarted = true;
        script->changed.notify_all();
        if (script->blockMake) {
            script->changed.wait(lock, [&script] { return script->releaseMake; });
        }
        if (script->failMake) {
            error = "no model file at test.onnx";
            failure = script->failure;
            return nullptr;
        }
        return std::make_unique<ScriptedReader>(script);
    };
}

// A reading of a passport page with ICAO's TD3 specimen: a band at the page's bottom, two lines.
inline doc_vision::MrzReading readingOf(const std::string& line1, const std::string& line2) {
    doc_vision::MrzReading reading;
    reading.band = doc_vision::Quad{{{60, 760}, {1190, 760}, {1190, 850}, {60, 850}}};
    reading.lines = {line1, line2};
    reading.peak = 0.9;
    reading.widthShare = 0.9;
    reading.aspect = 12.0;
    reading.detectTime = std::chrono::milliseconds(60);
    reading.recogniseTime = std::chrono::milliseconds(100);
    return reading;
}

// The same, with any number of lines: ICAO's TD1 card (three of 30), say.
inline doc_vision::MrzReading readingOf(std::vector<std::string> lines) {
    doc_vision::MrzReading reading = readingOf(std::string(), std::string());
    reading.lines = std::move(lines);
    return reading;
}

// ICAO Doc 9303's TD1 specimen (Part 5): the back of a Utopian identity card, no real person.
inline const std::vector<std::string> kTd1Specimen{
    "I<UTOD231458907<<<<<<<<<<<<<<<", "7408122F1204159UTO<<<<<<<<<<<6", "ERIKSSON<<ANNA<MARIA<<<<<<<<<<"};

// A reading that found no band.
inline doc_vision::MrzReading noBand() {
    doc_vision::MrzReading reading;
    reading.peak = 0.1;
    reading.detectTime = std::chrono::milliseconds(60);
    return reading;
}

} // namespace test_support
