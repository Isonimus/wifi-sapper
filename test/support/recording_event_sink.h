/**
 * @file recording_event_sink.h
 * @brief A test EventSink that records every AppEvent, copying its borrowed payload (ADR-0021).
 *
 * The bus delivers payloads by pointer, valid only for the dispatch call, so a recorder MUST copy them
 * — which is exactly what a real surface does. This sink copies each fact into a vector a test can then
 * assert against, so it doubles as a worked example of the copy-on-receive discipline.
 */
#pragma once

#include <cstddef>
#include <vector>

#include "core/event_bus.h"
#include "net/cracked_result.h"
#include "net/cracked_sync.h"        // SyncOutcome
#include "net/upload_supervisor.h"   // DrainOutcome

namespace sapper_test {

class RecordingEventSink : public sapper::EventSink {
public:
    // Sync facts.
    std::vector<sapper::CrackedResult> newPasswords;
    int summaries = 0;
    std::size_t lastSummaryCount = 0;
    std::vector<sapper::SyncOutcome> outcomes;
    // Drain facts.
    int drainStarted = 0;
    std::vector<sapper::DrainOutcome> drainsCompleted;
    // Capture facts.
    std::vector<sapper::CaptureFact> captures;

    void onAppEvent(const sapper::AppEvent& e) override {
        switch (e.type) {
            case sapper::AppEventType::NewPassword:
                newPasswords.push_back(*e.password);  // copy the borrowed payload, like a real surface.
                break;
            case sapper::AppEventType::HandshakeCaptured:
                captures.push_back(*e.capture);  // copy the borrowed identity-only payload (ADR-0031).
                break;
            case sapper::AppEventType::FirstSyncSummary:
                ++summaries;
                lastSummaryCount = e.importedCount;
                break;
            case sapper::AppEventType::SyncCompleted:
                outcomes.push_back(*e.sync);
                break;
            case sapper::AppEventType::DrainStarted:
                ++drainStarted;
                break;
            case sapper::AppEventType::DrainCompleted:
                drainsCompleted.push_back(*e.drain);
                break;
        }
    }
};

}  // namespace sapper_test
