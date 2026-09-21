/**
 * @file serial_command.cpp
 * @brief Pure line parser for the serial control channel (ADR-0003). Adapted from dupin.
 */
#include "hal/serial/serial_command.h"

#include <cstring>

namespace sapper {
namespace {

/// Length of @p s ignoring trailing spaces.
size_t trimmedLen(const char* s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') --n;
    return n;
}

/// Does @p line equal @p word, ignoring trailing spaces?
bool matchesWord(const char* line, const char* word) {
    const size_t wordLen = strlen(word);
    return trimmedLen(line) == wordLen && strncmp(line, word, wordLen) == 0;
}

}  // namespace

Command parseCommand(const char* line) {
    if (line == nullptr) return {};

    while (*line == ' ') ++line;
    if (*line == '\0') return {};

    if (matchesWord(line, "ping")) return {CommandKind::Ping};
    if (matchesWord(line, "state")) return {CommandKind::State};
    if (matchesWord(line, "dump")) return {CommandKind::Dump};

#ifdef SAPPER_TEST_HOOKS
    // The stimulus vocabulary, recognised only in test-hooks builds (ADR-0047, §4 #4). In the shipped
    // build these tokens fall through to Unknown below — the absence test_serial_command asserts.
    if (matchesWord(line, "inject-handshake")) return {CommandKind::InjectHandshake};
    if (matchesWord(line, "force-sync")) return {CommandKind::ForceSync};
    if (matchesWord(line, "inject-cracked")) return {CommandKind::InjectCracked};
    if (matchesWord(line, "inject-capture")) return {CommandKind::InjectCapture};
#endif

    return {CommandKind::Unknown};
}

bool CommandReader::feed(char c, Command& out) {
    if (c == '\r') return false;

    if (c != '\n') {
        if (m_len < kMaxCommandChars) {
            m_line[m_len++] = c;
        } else {
            // Past the bound the rest of the line is dropped, not stored. The flag survives
            // to the newline so the completed line is reported as refused rather than as
            // whatever the first kMaxCommandChars happened to spell.
            m_overflowed = true;
        }
        return false;
    }

    m_line[m_len] = '\0';
    out = m_overflowed ? Command{CommandKind::TooLong} : parseCommand(m_line);

    m_len = 0;
    m_overflowed = false;
    m_line[0] = '\0';
    return true;
}

}  // namespace sapper
