/**
 * @file wifi_station.cpp
 * @brief Station association and NTP clock sync (ADR-0006). Device-only.
 */
#ifndef UNIT_TEST

#include "net/wifi_station.h"

#include <WiFi.h>
#include <esp_sntp.h>

#include <ctime>

namespace sapper {
namespace {

// One association attempt's budget. Longer than a healthy DHCP lease negotiation, short enough
// that three attempts (kStaRetryBudget) fall back to the portal in well under a minute.
constexpr uint32_t kStaTimeoutMs = 15000;

// NTP sync budget. First sync can take a few seconds after association; bounded so a dead NTP
// path fails loud into a retry rather than hanging the boot.
constexpr uint32_t kNtpTimeoutMs = 15000;

// Poll interval while waiting on association / time — coarse enough not to busy-spin the CPU.
constexpr uint32_t kPollIntervalMs = 250;

// The default public NTP pool and UTC offsets. configTime(gmtOffset, dstOffset, server).
constexpr char kNtpServer[] = "pool.ntp.org";
constexpr long kGmtOffsetSec = 0;
constexpr int kDstOffsetSec = 0;

// A clock reading past this epoch (2020-01-01 00:00:00 UTC) is a real synced time, not the 1970
// power-on default. Below it, TLS certificate validity cannot be trusted (ADR-0006).
constexpr time_t kMinValidEpoch = 1577836800;

}  // namespace

bool connectStation(const char* ssid, const char* pass, BootTick tick) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start >= kStaTimeoutMs) {
            WiFi.disconnect(/*wifioff=*/true);  // release the radio before the caller retries.
            return false;
        }
        if (tick) tick();
        delay(kPollIntervalMs);
    }
    return true;
}

bool syncClock(BootTick tick) {
    // Idempotent, and it must be: the upload supervisor calls this on every drain (bringUpStation).
    // Re-running configTime() restarts the SNTP client, whose pending DNS callback then fires during
    // the upload's own DNS lookup and aborts in lwIP without the TCP/IP core lock held ("Required to
    // lock TCPIPcore functionality!", sys_untimeout). So once the clock is real, do nothing.
    if (std::time(nullptr) >= kMinValidEpoch) return true;

    configTime(kGmtOffsetSec, kDstOffsetSec, kNtpServer);
    const uint32_t start = millis();
    while (std::time(nullptr) < kMinValidEpoch) {
        if (millis() - start >= kNtpTimeoutMs) {
            esp_sntp_stop();  // stop the client so a later DNS lookup never services its callback.
            return false;
        }
        if (tick) tick();
        delay(kPollIntervalMs);
    }
    // Clock is set. Stop the SNTP service so its background re-sync DNS can never collide with an
    // upload's DNS on the app task (the crash above). A later deliberate re-sync (slice-6, sharing the
    // drain's STA session) re-runs configTime itself; for now the RTC free-runs, which is ample for
    // TLS validity (cert windows are days/months, drift is seconds).
    esp_sntp_stop();
    return true;
}

}  // namespace sapper

#endif  // UNIT_TEST
