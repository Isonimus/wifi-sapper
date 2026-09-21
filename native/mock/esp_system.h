/**
 * @file native/mock/esp_system.h
 * @brief Host-lane stand-in for ESP-IDF's <esp_system.h>, resolved via `-I native/mock` (ADR-0008).
 *
 * The native unit lane has no ESP-IDF, so the pure Maintenance reboot-intent decision (ADR-0057),
 * which is typed on `esp_reset_reason_t`, could not compile there without it. This fake defines only
 * that enum — the one piece the pure code touches — the same way `native/mock/Preferences.h` fakes
 * the NVS header. It is on the include path for the native env alone; device builds have no
 * `-I native/mock` and get the real ESP-IDF header.
 *
 * The enum values mirror ESP-IDF's documented public ABI for fidelity, but the pure decision compares
 * reset-reason *identity* (`reason == ESP_RST_SW`), never a numeric literal, so only the symbols need
 * to exist here — the device build supplies the true silicon values.
 */
#pragma once

/// Subset of ESP-IDF's reset causes, values matching the real header. Only the symbols the pure
/// reboot-intent decision and its tests name need be present; the list is the documented set so a
/// future host test can exercise any cause without guessing a value.
typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON = 1,
    ESP_RST_EXT = 2,
    ESP_RST_SW = 3,
    ESP_RST_PANIC = 4,
    ESP_RST_INT_WDT = 5,
    ESP_RST_TASK_WDT = 6,
    ESP_RST_WDT = 7,
    ESP_RST_DEEPSLEEP = 8,
    ESP_RST_BROWNOUT = 9,
    ESP_RST_SDIO = 10,
} esp_reset_reason_t;
