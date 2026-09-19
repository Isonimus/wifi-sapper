/**
 * @file fake_sync_session.h
 * @brief A scriptable SyncSession double for the supervisor's window-sharing tests (slice-0020
 *        Scenario H, integration half).
 *
 * Stands in for the whole scheduler+CrackedSync stack so the UploadSupervisor's window-sharing decision
 * is proved in isolation: whether a due sync opens a window on its own, piggybacks a drain, and only ever
 * runs inside a live (associated) window. `dueFlag` scripts due(); the counters record what the
 * supervisor did. Tests flip dueFlag from a test hook or leave it latched to model a persistent failure.
 */
#pragma once

#include <cstdint>

#include "net/sync_session.h"

namespace sapper_test {

class FakeSyncSession : public sapper::SyncSession {
public:
    bool dueFlag = false;   ///< What due() reports; a test sets it to model an owed sync.
    int beginCalls = 0;     ///< begin() calls (the supervisor seeds the cadence once, at begin()).
    int runCalls = 0;       ///< runInWindow() calls — each is one sync inside a live STA window.
    uint32_t lastRunMs = 0; ///< nowMs of the most recent runInWindow().

    void begin(uint32_t) override { ++beginCalls; }
    bool due(uint32_t) const override { return dueFlag; }
    void runInWindow(uint32_t nowMs) override {
        ++runCalls;
        lastRunMs = nowMs;
    }
};

}  // namespace sapper_test
