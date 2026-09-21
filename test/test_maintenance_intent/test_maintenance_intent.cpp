/**
 * @file test_maintenance_intent.cpp
 * @brief Native unit tests for the Maintenance reboot-intent + key predicate (ADR-0057, slice-0058).
 *
 * Proves slice-0058 Definition-of-Done Scenarios A and B:
 *  - A: `shouldEnterMaintenanceOnBoot`/`consumeMaintenanceIntent` honour the intent only on a software
 *       reset carrying the sentinel, and consume it one-shot (a re-read is false).
 *  - B: `isMaintenanceKey`/`decodeKeyEvent` recognise a *press* of the `m` key and nothing else.
 *
 * These are new pure units (there was no prior logic to regress): the fail-before is the missing
 * symbols — the suite does not link without the ADR-0057 pure half — and it passes once they exist.
 */
#include <unity.h>

#include "config/maintenance_intent.h"
#include "hal/input/tca8418_event.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

// --- Scenario A: reboot-intent -------------------------------------------------------------------

/// The sentinel is honoured only on a software reset: the exact reset ESP.restart() produces. Every
/// other cause (power-on, brownout, panic, watchdog, deep-sleep wake…) leaves the RTC word undefined,
/// so even a word that happens to equal the sentinel must be ignored — a power-cycled unit hunts.
void test_intent_honoured_only_on_software_reset(void) {
    TEST_ASSERT_TRUE(shouldEnterMaintenanceOnBoot(ESP_RST_SW, kMaintenanceIntentMagic));

    const esp_reset_reason_t nonSoftware[] = {
        ESP_RST_POWERON, ESP_RST_BROWNOUT, ESP_RST_UNKNOWN, ESP_RST_EXT,
        ESP_RST_PANIC,   ESP_RST_INT_WDT,  ESP_RST_TASK_WDT, ESP_RST_DEEPSLEEP,
    };
    for (esp_reset_reason_t reason : nonSoftware) {
        TEST_ASSERT_FALSE(shouldEnterMaintenanceOnBoot(reason, kMaintenanceIntentMagic));
    }
}

/// Even on a software reset, only the exact sentinel enters Maintenance — a cleared or garbage word
/// does not. Guards against a marginal reset that reports ESP_RST_SW but left junk in the RTC word.
void test_intent_requires_the_sentinel(void) {
    TEST_ASSERT_FALSE(shouldEnterMaintenanceOnBoot(ESP_RST_SW, kMaintenanceIntentCleared));
    TEST_ASSERT_FALSE(shouldEnterMaintenanceOnBoot(ESP_RST_SW, kMaintenanceIntentMagic + 1u));
    TEST_ASSERT_FALSE(shouldEnterMaintenanceOnBoot(ESP_RST_SW, 0xFFFFFFFFu));
    TEST_ASSERT_FALSE(shouldEnterMaintenanceOnBoot(ESP_RST_SW, 0x4E49414Du));  // byte-swapped sentinel
}

/// The read is one-shot: consuming a live intent yields the cleared word, and re-evaluating with that
/// cleared word — even on another software reset — is false. This is what stops a crash inside
/// Maintenance (which reboots as ESP_RST_SW) from looping the device back into Maintenance.
void test_consume_is_one_shot(void) {
    const MaintenanceIntentRead first = consumeMaintenanceIntent(ESP_RST_SW, kMaintenanceIntentMagic);
    TEST_ASSERT_TRUE(first.enterMaintenance);
    TEST_ASSERT_EQUAL_UINT32(kMaintenanceIntentCleared, first.clearedWord);

    const MaintenanceIntentRead second = consumeMaintenanceIntent(ESP_RST_SW, first.clearedWord);
    TEST_ASSERT_FALSE(second.enterMaintenance);
    TEST_ASSERT_EQUAL_UINT32(kMaintenanceIntentCleared, second.clearedWord);
}

/// Consuming when there is no intent (a normal power-on boot) reports no entry and still yields the
/// cleared word, so the caller's unconditional write-back leaves the RTC word in the "no intent" state.
void test_consume_without_intent_clears_and_declines(void) {
    const MaintenanceIntentRead read = consumeMaintenanceIntent(ESP_RST_POWERON, 0xDEADBEEFu);
    TEST_ASSERT_FALSE(read.enterMaintenance);
    TEST_ASSERT_EQUAL_UINT32(kMaintenanceIntentCleared, read.clearedWord);
}

// --- Scenario B: key predicate -------------------------------------------------------------------

// Build the raw TCA8418 FIFO byte for a given 1-based key number and state — the inverse of the
// datasheet decode (bit7 = press, low 7 = key number). Lets each case name intent, not a bare hex.
static constexpr uint8_t rawEvent(uint8_t keyNumber, bool pressed) {
    return static_cast<uint8_t>((pressed ? 0x80 : 0x00) | (keyNumber & 0x7F));
}

/// Only a *press* of the Maintenance key counts: its release, other keys (pressed or released), and
/// the empty event `0x00` are all rejected — so a key-up never enters Maintenance and no neighbouring
/// key is mistaken for `m`.
void test_only_maintenance_key_press_matches(void) {
    TEST_ASSERT_TRUE(isMaintenanceKey(rawEvent(kMaintenanceKeycode, /*pressed=*/true)));

    TEST_ASSERT_FALSE(isMaintenanceKey(rawEvent(kMaintenanceKeycode, /*pressed=*/false)));  // release
    TEST_ASSERT_FALSE(isMaintenanceKey(rawEvent(kMaintenanceKeycode - 1, true)));           // adjacent key
    TEST_ASSERT_FALSE(isMaintenanceKey(rawEvent(kMaintenanceKeycode + 1, true)));           // adjacent key
    TEST_ASSERT_FALSE(isMaintenanceKey(rawEvent(kMaintenanceKeycode + 1, false)));
    TEST_ASSERT_FALSE(isMaintenanceKey(0x00));  // empty FIFO event
}

/// The `m` press byte is `0x80 | 48 = 0xB0` and its release is `0x30`, pinning the encoding to a
/// concrete number the on-device probe (`verify:keyboard`) then confirms physically (ADR-0057 #5).
void test_maintenance_key_byte_encoding(void) {
    TEST_ASSERT_TRUE(isMaintenanceKey(0xB0));
    TEST_ASSERT_FALSE(isMaintenanceKey(0x30));
}

/// decodeKeyEvent splits a byte into press/release, the raw 1-based key number, and the 0-based
/// electrical row/col. `m` (key number 48) sits at electrical row 4, col 7; the empty event is key 0.
void test_decode_splits_state_and_position(void) {
    const KeyEvent press = decodeKeyEvent(0xB0);
    TEST_ASSERT_TRUE(press.pressed);
    TEST_ASSERT_EQUAL_UINT8(48, press.keycode);
    TEST_ASSERT_EQUAL_UINT8(4, press.row);  // (48-1)/10
    TEST_ASSERT_EQUAL_UINT8(7, press.col);  // (48-1)%10

    const KeyEvent release = decodeKeyEvent(0x30);
    TEST_ASSERT_FALSE(release.pressed);
    TEST_ASSERT_EQUAL_UINT8(48, release.keycode);

    const KeyEvent empty = decodeKeyEvent(0x00);
    TEST_ASSERT_FALSE(empty.pressed);
    TEST_ASSERT_EQUAL_UINT8(0, empty.keycode);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_intent_honoured_only_on_software_reset);
    RUN_TEST(test_intent_requires_the_sentinel);
    RUN_TEST(test_consume_is_one_shot);
    RUN_TEST(test_consume_without_intent_clears_and_declines);
    RUN_TEST(test_only_maintenance_key_press_matches);
    RUN_TEST(test_maintenance_key_byte_encoding);
    RUN_TEST(test_decode_splits_state_and_position);
    return UNITY_END();
}
