#include "lab_core/mrz_worker.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace lab_core {

namespace {

// Finished reads the processor has not taken yet are kept up to this many, the oldest dropped first: it takes
// them every frame, so more only pile up when no frames come.
constexpr std::size_t kDoneKept = 4;

} // namespace

MrzWorker::MrzWorker(Mode mode, MakeReader makeReader) : m_mode(mode), m_makeReader(std::move(makeReader)) {
    if (m_mode == Mode::Threaded) {
        m_thread = std::thread([this] { run(); });
    }
}

MrzWorker::~MrzWorker() {
    {
        const std::lock_guard lock(m_mutex);
        m_quit = true;
        m_waiting.reset();
        if (m_reader) {
            m_reader->cancel(); // a read in progress ends now; one still opening is never used
        }
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MrzWorker::submit(MrzJob job) {
    {
        const std::lock_guard lock(m_mutex);
        if (m_unavailable || m_quit) {
            return;
        }
        if (m_mode != Mode::Immediate) {
            m_waiting = std::move(job); // a job still waiting goes, and its frame with it
            m_wake.notify_all();
            return;
        }
        m_working = true;
    }
    work(std::move(job));
    const std::lock_guard lock(m_mutex);
    m_working = false;
}

std::vector<MrzDone> MrzWorker::take() {
    std::vector<MrzDone> done;
    const std::lock_guard lock(m_mutex);
    done.swap(m_done);
    return done;
}

void MrzWorker::forget(std::uint64_t resetCount) {
    std::optional<MrzJob> waiting;
    std::vector<MrzDone> done;
    {
        const std::lock_guard lock(m_mutex);
        waiting.swap(m_waiting);
        done.swap(m_done);
        m_keepFrom = std::max(m_keepFrom, resetCount);
    }
    // Their frames go here, outside the lock.
}

bool MrzWorker::pump() {
    std::optional<MrzJob> job;
    {
        const std::lock_guard lock(m_mutex);
        if (m_mode != Mode::Manual || !m_waiting) {
            return false;
        }
        job = std::move(m_waiting);
        m_waiting.reset();
        m_working = true;
    }
    work(std::move(*job));
    const std::lock_guard lock(m_mutex);
    m_working = false;
    return true;
}

std::optional<std::string> MrzWorker::unavailable() const {
    const std::lock_guard lock(m_mutex);
    return m_unavailable;
}

bool MrzWorker::busy() const {
    const std::lock_guard lock(m_mutex);
    return m_waiting.has_value() || m_working;
}

void MrzWorker::run() {
    for (;;) {
        std::optional<MrzJob> job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quit || m_waiting.has_value(); });
            if (m_quit) {
                return;
            }
            job = std::move(m_waiting);
            m_waiting.reset();
            m_working = true;
        }
        work(std::move(*job));
        const std::lock_guard lock(m_mutex);
        m_working = false;
    }
}

void MrzWorker::work(MrzJob job) {
    std::shared_ptr<doc_vision::MrzReader> reader;
    bool make = false;
    {
        const std::lock_guard lock(m_mutex);
        if (m_quit || m_unavailable) {
            return;
        }
        reader = m_reader;
        make = !reader && !m_tried;
    }
    if (make) {
        std::string error;
        doc_vision::OpenFailure failure = doc_vision::OpenFailure::None;
        std::unique_ptr<doc_vision::MrzReader> made;
        try {
            if (m_makeReader) {
                made = m_makeReader(error, failure);
            }
        } catch (...) {
            made.reset();
        }
        const std::lock_guard lock(m_mutex);
        m_tried = true;
        if (!made) {
            m_unavailable =
                failure == doc_vision::OpenFailure::Runtime ? "ONNX Runtime refused it" : "model missing";
            return;
        }
        m_reader = std::move(made);
        reader = m_reader;
        if (m_quit) {
            return; // the destructor came while the reader opened: it is not used
        }
    }
    if (!reader) {
        return;
    }
    MrzDone done;
    done.resetCount = job.resetCount;
    try {
        const cv::Mat page = doc_vision::flatten(job.bgr, job.record.quad, doc_vision::kPageSize);
        job.bgr.release();
        done.reading = doc_vision::readUpright(*reader, page, job.turnFirst, done.turned, done.error);
    } catch (...) {
        done.reading = {};
        done.turned = false;
        done.error = "the reader failed";
    }
    done.record = std::move(job.record);
    const std::lock_guard lock(m_mutex);
    if (done.resetCount < m_keepFrom) {
        return; // forgotten while it was read; its frame goes after the lock
    }
    m_done.push_back(std::move(done));
    while (m_done.size() > kDoneKept) {
        m_done.erase(m_done.begin());
    }
}

} // namespace lab_core
