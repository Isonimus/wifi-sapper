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

namespace sapper {

class SerialChannel {
public:
    explicit SerialChannel(const IDisplay& display) : m_display(display) {}

    /// Print the boot banner as a [STATE] line so a just-flashed device announces itself.
    void announce() const;

    /// Drain up to a bounded number of input bytes, dispatching each completed line.
    void pump();

private:
    void dispatch(const Command& command) const;
    void reportState() const;
    void streamDump() const;

    const IDisplay& m_display;
    CommandReader m_reader;
};

}  // namespace sapper
