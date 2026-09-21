/**
 * @file test_serial_command.cpp
 * @brief Native unit tests for the pure serial-command parser (ADR-0003, ADR-0004 lane 1).
 *
 * Proves slice-0005 Definition-of-Done Scenario A (parser half) and the parse side of
 * Scenario D: the observation vocabulary is exactly ping/state/dump, over-length lines are
 * refused rather than truncated, and the accumulator resets itself per line.
 */
#include <unity.h>

#include <string>

#include "hal/serial/serial_command.h"

using namespace sapper;

static int kindOf(const char* line) { return static_cast<int>(parseCommand(line).kind); }

void setUp(void) {}
void tearDown(void) {}

// --- parseCommand: the vocabulary -------------------------------------------

void test_ping_state_dump_recognised(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Ping), kindOf("ping"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::State), kindOf("state"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Dump), kindOf("dump"));
}

void test_surrounding_spaces_ignored(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Ping), kindOf("  ping  "));
}

void test_blank_and_null_are_none(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::None), kindOf(""));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::None), kindOf("   "));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::None), kindOf(nullptr));
}

void test_unknown_and_partial_words_refused(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("pin"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("pingg"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("deauth"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("PING"));
}

// The §4 #4 teeth (ADR-0047): the stimulus vocabulary is gated by SAPPER_TEST_HOOKS. The native unit
// lane compiles serial_command.cpp WITHOUT the flag — the *shipped* form — so on every run this proves
// the released parser rejects every actuation token. Moving any token's parse arm (or its enumerator)
// outside the #ifdef makes this no-hooks build recognise it and this assertion fails. Under a hooks
// build the same test proves the tokens parse, so it is correct whichever way the file is compiled.
void test_stimulus_vocabulary_gated_by_hooks(void) {
#ifdef SAPPER_TEST_HOOKS
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::InjectHandshake), kindOf("inject-handshake"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::ForceSync), kindOf("force-sync"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::InjectCracked), kindOf("inject-cracked"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::InjectCapture), kindOf("inject-capture"));
#else
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("inject-handshake"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("force-sync"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("inject-cracked"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Unknown), kindOf("inject-capture"));
#endif
}

// --- CommandReader: accumulation and bounds ---------------------------------

static CommandKind feedLine(CommandReader& reader, const std::string& bytes) {
    Command out;
    CommandKind last = CommandKind::None;
    for (char c : bytes) {
        if (reader.feed(c, out)) last = out.kind;
    }
    return last;
}

void test_reader_completes_on_newline(void) {
    CommandReader reader;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::State),
                          static_cast<int>(feedLine(reader, "state\n")));
}

void test_reader_ignores_cr_so_crlf_is_lf(void) {
    CommandReader reader;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Ping),
                          static_cast<int>(feedLine(reader, "ping\r\n")));
}

void test_reader_resets_between_lines(void) {
    CommandReader reader;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Ping),
                          static_cast<int>(feedLine(reader, "ping\n")));
    // A second line parses on its own, not as a continuation of the first.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Dump),
                          static_cast<int>(feedLine(reader, "dump\n")));
}

void test_overlong_line_refused_not_truncated(void) {
    CommandReader reader;
    // A line longer than the bound must report TooLong, never a prefix that spells a command.
    std::string flood(kMaxCommandChars + 5, 'x');
    flood += '\n';
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::TooLong),
                          static_cast<int>(feedLine(reader, flood)));
}

void test_overflow_flag_clears_for_next_line(void) {
    CommandReader reader;
    std::string flood(kMaxCommandChars + 2, 'x');
    flood += '\n';
    feedLine(reader, flood);  // TooLong
    // The next well-formed line is unaffected by the previous overflow.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Ping),
                          static_cast<int>(feedLine(reader, "ping\n")));
}

void test_overlong_stimulus_prefix_refused(void) {
    CommandReader reader;
    // A line that begins with an actuation token but runs past the bound must be refused whole, never
    // truncated back into the command a prefix spells — the property matters most for the stimulus
    // vocabulary, since a shortened actuation command "would run something unasked" (ADR-0003 #5).
    std::string line = "inject-handshake";
    line += std::string(kMaxCommandChars, 'x');  // push well past kMaxCommandChars.
    line += '\n';
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::TooLong),
                          static_cast<int>(feedLine(reader, line)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_ping_state_dump_recognised);
    RUN_TEST(test_surrounding_spaces_ignored);
    RUN_TEST(test_blank_and_null_are_none);
    RUN_TEST(test_unknown_and_partial_words_refused);
    RUN_TEST(test_stimulus_vocabulary_gated_by_hooks);
    RUN_TEST(test_reader_completes_on_newline);
    RUN_TEST(test_reader_ignores_cr_so_crlf_is_lf);
    RUN_TEST(test_reader_resets_between_lines);
    RUN_TEST(test_overlong_line_refused_not_truncated);
    RUN_TEST(test_overflow_flag_clears_for_next_line);
    RUN_TEST(test_overlong_stimulus_prefix_refused);
    return UNITY_END();
}
