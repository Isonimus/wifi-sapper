/**
 * @file event_bus.h
 * @brief The surface event bus: a synchronous, heap-free multicast of engine facts (ADR-0021).
 *
 * ADR-0001 made every user-facing surface an observer of engine facts, never an owner of engine
 * control flow, and named a shared bus as the mechanism. This is that bus. A producer (the upload
 * supervisor, the cracked-results sync) publishes an AppEvent; every subscribed EventSink sees it,
 * synchronously, in subscription order, on the app task. A surface implements one method,
 * onAppEvent(), subscribes once, and reacts to every fact it cares about — so a status LED that
 * must reflect hunting *and* uploading *and* a recovered password is one subscription, not three
 * observer interfaces (ADR-0021: the rule-of-three trigger this bus resolves).
 *
 * What is NOT here: the capture→queue data path. A captured handshake flowing to the supervisor is a
 * must-deliver single-consumer pipe (a dropped one is a lost handshake), so it stays a direct
 * CaptureReadyObserver seam and is never a best-effort broadcast on this bus (ADR-0021 boundary; §4
 * invariant #13). The bus carries facts a surface may observe or ignore, not data the engine must
 * deliver.
 *
 * The whole thing is pure — no hardware, no heap, no RTTI — so it is host-tested on the native lane,
 * and payloads are borrowed for the dispatch call only (see AppEvent).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

// Forward declarations only: AppEvent references these payloads by const pointer, so the bus header
// pulls in none of the heavy producer headers. A sink dereferences a payload in its own .cpp, where
// it includes the full definition. This is what keeps the include graph acyclic — cracked_sync.h and
// upload_supervisor.h include THIS header to publish, not the other way round.
struct DrainOutcome;
struct SyncOutcome;
struct CrackedResult;

/// The kind of fact an AppEvent carries; the sink switches on it to read the right payload.
enum class AppEventType : uint8_t {
    DrainStarted,      ///< An STA window opened — the appliance is off-air, associating/uploading.
    DrainCompleted,    ///< A drain cycle finished; payload `drain` carries its counted outcome.
    SyncCompleted,     ///< A cracked-results sync finished; payload `sync` carries its outcome.
    NewPassword,       ///< A genuinely new/changed crack in steady state; payload `password`.
    FirstSyncSummary,  ///< A fresh manifest was seeded; `importedCount` results, announced once.
};

/**
 * @brief One engine fact. Payloads are borrowed for the dispatch call only.
 *
 * The pointer members alias a struct the producer owns and holds live across the synchronous
 * publish() call; they are valid ONLY for the duration of onAppEvent(). A sink that needs the data
 * afterwards must copy it (the LED latches a copied state, never the pointer). This keeps AppEvent
 * tiny and heap-free — correct precisely because dispatch is synchronous (ADR-0021).
 *
 * Construct through the named factories, never by setting fields ad hoc: each factory sets exactly
 * the field its type reads, so a sink can trust that e.g. a DrainCompleted event has a non-null
 * `drain`.
 */
struct AppEvent {
    AppEventType type;
    const DrainOutcome* drain = nullptr;      ///< Non-null iff type == DrainCompleted.
    const SyncOutcome* sync = nullptr;        ///< Non-null iff type == SyncCompleted.
    const CrackedResult* password = nullptr;  ///< Non-null iff type == NewPassword.
    size_t importedCount = 0;                 ///< Meaningful iff type == FirstSyncSummary.

    static AppEvent drainStarted() { return AppEvent{AppEventType::DrainStarted}; }
    static AppEvent drainCompleted(const DrainOutcome& outcome) {
        AppEvent e{AppEventType::DrainCompleted};
        e.drain = &outcome;
        return e;
    }
    static AppEvent syncCompleted(const SyncOutcome& outcome) {
        AppEvent e{AppEventType::SyncCompleted};
        e.sync = &outcome;
        return e;
    }
    static AppEvent newPassword(const CrackedResult& result) {
        AppEvent e{AppEventType::NewPassword};
        e.password = &result;
        return e;
    }
    static AppEvent firstSyncSummary(size_t importedCount) {
        AppEvent e{AppEventType::FirstSyncSummary};
        e.importedCount = importedCount;
        return e;
    }
};

/// A surface. Implement onAppEvent, subscribe to the bus, react to the facts you care about and
/// ignore the rest. A sink must NOT publish from within onAppEvent — surfaces observe, never produce
/// (ADR-0021; the fan-out is a bare loop with no re-entrancy guard).
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void onAppEvent(const AppEvent& event) = 0;
};

/**
 * @brief Fixed-capacity synchronous multicast of AppEvents to subscribed sinks.
 *
 * Boot wiring calls subscribe() once per surface, then producers call publish() for the device's
 * whole run. No heap: the sink table is a fixed array sized to the known surface count. Exceeding it
 * is a wiring error caught loud at boot (subscribe returns false; the caller treats it as fatal),
 * never a silently dropped surface (quality bar §3).
 */
class EventBus {
public:
    /// Register @p sink to receive every published event. Returns false (loud) if the bus is full or
    /// @p sink is already subscribed — either is a boot-time wiring bug the caller must surface.
    bool subscribe(EventSink& sink) {
        if (count_ >= kMaxSinks) return false;
        for (size_t i = 0; i < count_; ++i) {
            if (sinks_[i] == &sink) return false;  // double-subscribe would double-deliver every event.
        }
        sinks_[count_++] = &sink;
        return true;
    }

    /// Deliver @p event to every subscribed sink synchronously, in subscription order. const because
    /// publishing mutates no bus state — the sinks react, the bus only routes.
    void publish(const AppEvent& event) const {
        for (size_t i = 0; i < count_; ++i) sinks_[i]->onAppEvent(event);
    }

    size_t subscriberCount() const { return count_; }

private:
    /// Reserves the four planned slice-7 surfaces (LED, web, toast, webhook) plus a serial logger,
    /// with one spare. A sixth/seventh surface is a one-line bump plus a re-measure of the tiny
    /// static cost (ADR-0021).
    static constexpr size_t kMaxSinks = 6;

    EventSink* sinks_[kMaxSinks] = {};
    size_t count_ = 0;
};

}  // namespace sapper
