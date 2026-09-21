/**
 * @file serial_channel.cpp
 * @brief Device-side dispatch for the serial control channel (ADR-0003).
 */
#include "hal/serial/serial_channel.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstdio>

#ifdef SAPPER_TEST_HOOKS
// Driving the engine and network flows is the stimulus channel's stated purpose (ADR-0003). The
// include and the dispatch arms that use it are confined to this test-hooks block, so the *shipped*
// serial_channel.cpp never pulls in the hunt spine and no released binary carries an actuation path
// (§4 #4). The injectors dispatch through the same seams a real event uses (§4 #2; ADR-0047).
#include "hunt_loop.h"
#endif

namespace sapper {
namespace {

// Bytes drained from the port per pump() call. Bounded so a flood or a wedged sender cannot
// starve the rest of the loop (ADR-0003 #5); the vocabulary is far shorter than one drain.
constexpr size_t kMaxDrainBytes = 64;

// Canvas bytes hexed per output line during a dump — 32 bytes -> 64 hex chars per line.
constexpr size_t kDumpChunkBytes = 32;

}  // namespace

void SerialChannel::announce() const { reportState(); }

void SerialChannel::pump() {
    for (size_t drained = 0; drained < kMaxDrainBytes && Serial.available() > 0; ++drained) {
        Command command;
        if (m_reader.feed(static_cast<char>(Serial.read()), command)) {
            dispatch(command);
        }
    }
}

void SerialChannel::dispatch(const Command& command) const {
    switch (command.kind) {
        case CommandKind::None:
            break;  // bare newline: not an error, nothing to answer
        case CommandKind::Ping:
            Serial.println("[CMD] pong");
            break;
        case CommandKind::State:
            reportState();
            break;
        case CommandKind::Dump:
            streamDump();
            break;
        case CommandKind::Unknown:
            Serial.println("[CMD] refused unknown-command");
            break;
        case CommandKind::TooLong:
            Serial.println("[CMD] refused line-too-long");
            break;
#ifdef SAPPER_TEST_HOOKS
        // The stimulus arms (ADR-0047). Each acks with a [CMD] line (ADR-0003 #6 — never [ERROR]) and
        // calls the matching huntLoop injector, which drives the running loop through a real seam
        // (§4 #2) and prints its own [UPLOAD]/[SYNC]/[CRACK]/[CAPTURE] evidence. Each injector is a
        // no-op before the hunt loop reaches Ready (it guards on g_running), so a premature stimulus is
        // harmless. These enum values exist only under SAPPER_TEST_HOOKS, so the shipped switch above is
        // exhaustive without them.
        case CommandKind::InjectHandshake:
            Serial.println("[CMD] inject-handshake");
            huntLoopInjectStimulus();
            break;
        case CommandKind::ForceSync:
            Serial.println("[CMD] force-sync");
            huntLoopForceSyncDue();
            break;
        case CommandKind::InjectCracked:
            Serial.println("[CMD] inject-cracked");
            huntLoopInjectCrackedAlert();
            break;
        case CommandKind::InjectCapture:
            Serial.println("[CMD] inject-capture");
            huntLoopInjectCaptureAlert();
            break;
#endif
    }
}

void SerialChannel::reportState() const {
    // Fixed buffer, no String: reporting state must allocate nothing, so a second `state`
    // reports the same free heap and evidences the observation path is read-only (ADR-0003
    // invariant #1; slice-0005 Scenario D).
    char line[96];
    std::snprintf(line, sizeof(line),
                  "[STATE] phase=%s heap_free=%u heap_max=%u disp=%ux%u",
                  phaseLabel(m_phase),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(m_display.width()),
                  static_cast<unsigned>(m_display.height()));
    Serial.println(line);
}

void SerialChannel::streamDump() const {
    const size_t byteLen = m_display.canvasByteLength();
    char header[64];
    std::snprintf(header, sizeof(header), "[DUMP] begin w=%u h=%u bpp=16 bytes=%u",
                  static_cast<unsigned>(m_display.width()),
                  static_cast<unsigned>(m_display.height()),
                  static_cast<unsigned>(byteLen));
    Serial.println(header);

    uint8_t chunk[kDumpChunkBytes];
    char hex[kDumpChunkBytes * 2 + 1];
    for (size_t offset = 0;;) {
        const size_t n = m_display.readCanvas(offset, chunk, sizeof(chunk));
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", chunk[i]);
        }
        Serial.println(hex);
        offset += n;
    }
    Serial.println("[DUMP] end");
}

}  // namespace sapper
