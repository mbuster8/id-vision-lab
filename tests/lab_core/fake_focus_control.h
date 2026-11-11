#pragma once

// A camera focus control for the tests (A3.3): it records every call and moves a pretend lens, and the tests
// read and steer it through a FocusLog they share with it. Threaded tests read the log only once the engine
// that called it is destroyed (its destructor joins the processing thread).

#include "lab_core/focus_control.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace test_support {

struct FocusLog {
    // What the control says and does.
    std::optional<lab_core::FocusRange> range = lab_core::FocusRange{.minimum = 0, .maximum = 250, .step = 5};
    bool hasAuto = true;
    bool autoFocus = true;
    int position = 120;
    bool positionReadable = true;
    bool failSetPosition = false;
    bool failSetAuto = false;
    // Every call in order: "range", "has auto", "auto?", "auto on", "auto off", "position?", "position 30".
    std::vector<std::string> calls{};
    // The `at` of the frame being processed, set by the test, so a move knows when it was made.
    std::chrono::nanoseconds now{};
    struct Move {
        std::chrono::nanoseconds at{};
        int from = 0;
        int to = 0;
    };
    std::vector<Move> moves{};
    std::thread::id lastThread{};
    bool destroyed = false;
    std::thread::id destroyedOn{};

    int count(const std::string& call) const {
        int n = 0;
        for (const std::string& c : calls) {
            n += c == call ? 1 : 0;
        }
        return n;
    }
};

class FakeFocusControl final : public lab_core::FocusControl {
public:
    explicit FakeFocusControl(std::shared_ptr<FocusLog> log) : m_log(std::move(log)) {}
    ~FakeFocusControl() override {
        m_log->destroyed = true;
        m_log->destroyedOn = std::this_thread::get_id();
    }

    std::optional<lab_core::FocusRange> range() noexcept override {
        note("range");
        return m_log->range;
    }
    bool hasAutoFocus() noexcept override {
        note("has auto");
        return m_log->hasAuto;
    }
    std::optional<bool> autoFocus() noexcept override {
        note("auto?");
        return m_log->hasAuto ? std::optional<bool>(m_log->autoFocus) : std::nullopt;
    }
    bool setAutoFocus(bool on) noexcept override {
        note(on ? "auto on" : "auto off");
        if (m_log->failSetAuto) {
            return false;
        }
        m_log->autoFocus = on;
        return true;
    }
    std::optional<int> position() noexcept override {
        note("position?");
        return m_log->positionReadable ? std::optional<int>(m_log->position) : std::nullopt;
    }
    bool setPosition(int position) noexcept override {
        note("position " + std::to_string(position));
        if (m_log->failSetPosition) {
            return false;
        }
        m_log->moves.push_back({.at = m_log->now, .from = m_log->position, .to = position});
        m_log->position = position;
        return true;
    }

private:
    void note(const std::string& call) {
        m_log->calls.push_back(call);
        m_log->lastThread = std::this_thread::get_id();
    }

    std::shared_ptr<FocusLog> m_log;
};

} // namespace test_support
