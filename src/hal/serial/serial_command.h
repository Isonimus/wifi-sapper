/**
 * @file serial_command.h
 * @brief Pure line parser for the serial control channel's observation half (ADR-0003).
 *
 * Adapted from dupin's `ui/serial_command.h`. The parsing is pure and host-tested; the
 * dispatch that touches the port, the heap, or the canvas lives in `serial_channel` and is
 * covered by the device verify script (ADR-0003 #4).
 *
 * This half is the *observation* vocabulary only — `ping`, `state`, `dump` — which ships
 * unflagged in every build and mutates no engine state (ADR-0003 #2, invariant #1). The
 * stimulus vocabulary is absent here by design: it arrives behind `SAPPER_TEST_HOOKS` with
 * the engine it drives, so no shipped binary can parse a command that transmits.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

// Longest command line accepted, excluding the terminator. The vocabulary is far shorter;
// the bound exists so a flood or a wedged sender cannot grow the buffer, and a line past it
// is refused rather than truncated — truncating a longer word into a shorter valid one would
// run a command the sender did not ask for (ADR-0003 #5).
constexpr size_t kMaxCommandChars = 32;

enum class CommandKind : uint8_t {
    None,     ///< Blank line. Not an error; a bare newline is how a terminal user probes.
    Ping,     ///< Liveness. Reply: [CMD] pong.
    State,    ///< Report observable state (engine phase, heap, geometry). Read-only.
    Dump,     ///< Stream the canvas buffer back for the verify artifact. Read-only.
    Unknown,  ///< A line that is not a command.
    TooLong,  ///< A line past kMaxCommandChars, refused rather than truncated.
};

struct Command {
    CommandKind kind = CommandKind::None;
};

/**
 * @brief Parse one complete line. Pure: no port, no heap, no allocation.
 *
 * Accepts exactly `ping`, `state`, `dump`; leading and trailing spaces are ignored and
 * everything else is `Unknown`. Matching is exact and lower-case: the caller is a script,
 * not a person, and a gate that guesses at intent can pass for the wrong reason.
 */
Command parseCommand(const char* line);

/**
 * @brief Accumulates bytes into lines and parses each one.
 *
 * feed() returns true exactly when @p out has been filled, resetting itself in the same step.
 * `\r` is ignored so a CRLF terminal behaves like a bare-LF script. A line longer than
 * kMaxCommandChars is reported as `TooLong` at its terminator, never as a truncated command.
 */
class CommandReader {
public:
    /** @brief Consume one byte. Returns true when a line completed and @p out was filled. */
    bool feed(char c, Command& out);

private:
    char m_line[kMaxCommandChars + 1] = {0};
    size_t m_len = 0;
    bool m_overflowed = false;
};

}  // namespace sapper
