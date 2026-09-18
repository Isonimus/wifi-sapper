/**
 * @file serial_channel.h
 * @brief Device-side dispatch for the serial control channel's observation half (ADR-0003).
 *
 * Device-only: touches the port, the heap counters, and the canvas. The parsing it drives is
 * the pure `serial_command` unit; this half is what the device verify script exercises
 * (ADR-0003 #4). Every command here is read-only — it reports what the device already exposes
 * and mutates no engine state (ADR-0003 invariant #1).
 *
 * `pump()` drains a bounded number of input bytes per call so a serial flood cannot starve
 * the rest of the loop (ADR-0003 #5), and any pre-engine blocking state must call it so a
 * freshly-flashed device stays observable (ADR-0003 #7).
 */
#pragma once

#include "hal/display/display_hal.h"
#include "hal/serial/serial_command.h"
#include "net/provisioning.h"  // Phase — reported live on the [STATE] line (ADR-0006 #3)

namespace sapper {

class SerialChannel {
public:
    /// @param phase The live boot phase, owned by main.cpp and read by reference so `[STATE]`
    ///        always reports the true phase rather than a value captured at construction.
    SerialChannel(const IDisplay& display, const Phase& phase)
        : m_display(display), m_phase(phase) {}

    /// Print the boot banner as a [STATE] line so a just-flashed device announces itself.
    void announce() const;

    /// Drain up to a bounded number of input bytes, dispatching each completed line.
    void pump();

private:
    void dispatch(const Command& command) const;
    void reportState() const;
    void streamDump() const;

    const IDisplay& m_display;
    const Phase& m_phase;
    CommandReader m_reader;
};

}  // namespace sapper
