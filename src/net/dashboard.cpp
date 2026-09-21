/**
 * @file dashboard.cpp
 * @brief Pure Maintenance-dashboard rendering + backstop (ADR-0039). Host-tested; no Arduino.
 */
#include "net/dashboard.h"

#include <cstdio>
#include <cstring>

namespace sapper {
namespace {

/**
 * @brief HTML-escape @p src (bounded to @p srcCap bytes) into @p out. The ESSID and PSK come from the
 *        air / an external service, so `<`, `>`, `&`, `"`, `'` are turned into entities — a crafted
 *        ESSID like "<script>" then renders as text, never as markup, on the served page.
 * @return true on success; false if the escaped form does not fit @p outSize (fail loud, no truncation).
 */
bool escapeHtml(const char* src, size_t srcCap, char* out, size_t outSize) {
    size_t o = 0;
    const size_t len = strnlen(src, srcCap);
    for (size_t i = 0; i < len; ++i) {
        const char* entity = nullptr;
        switch (src[i]) {
            case '&': entity = "&amp;"; break;
            case '<': entity = "&lt;"; break;
            case '>': entity = "&gt;"; break;
            case '"': entity = "&quot;"; break;
            case '\'': entity = "&#39;"; break;
            default: break;
        }
        if (entity != nullptr) {
            const size_t elen = std::strlen(entity);
            if (o + elen >= outSize) return false;  // no room for the entity + a later NUL.
            std::memcpy(out + o, entity, elen);
            o += elen;
        } else {
            if (o + 1 >= outSize) return false;  // no room for the char + a later NUL.
            out[o++] = src[i];
        }
    }
    if (o >= outSize) return false;
    out[o] = '\0';
    return true;
}

}  // namespace

size_t buildDashboardHead(const DashboardStats& stats, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return 0;

    const int written = std::snprintf(
        out, outSize,
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi Sapper - Maintenance</title>"
        "<style>body{font-family:sans-serif;margin:1rem}table{border-collapse:collapse;width:100%%}"
        "th,td{border:1px solid #ccc;padding:.4rem;text-align:left}"
        "code{word-break:break-all}.warn{color:#a00}</style></head><body>"
        "<h2>WiFi Sapper - Maintenance</h2>"
        "<ul>"
        "<li>Recovered networks: %zu</li>"
        "<li>Captures queued for upload: %zu</li>"
        "<li>Deauth: %s</li>"
        "<li>Sync history: %s</li>"
        "</ul>"
        "<p class='warn'>Recovered passwords are shown in clear below. This viewer is served over the "
        "device's own maintenance access point (ADR-0039).</p>"
        "<table><tr><th>Network</th><th>Password</th></tr>",
        stats.recoveredCount, stats.captureQueueDepth, stats.deauthArmed ? "ARMED" : "disarmed",
        stats.everSynced ? "completed at least once" : "never synced");

    // snprintf returns what it *would* have written: >= outSize means it truncated. Serve nothing
    // rather than a half-page (§3, fail loud); clear out[0] so a careless caller cannot send garbage.
    if (written < 0 || static_cast<size_t>(written) >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

size_t appendCrackedRow(const CrackedResult& result, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return 0;

    // Escape both fields first: they arrive from wpa-sec / the air, so a crafted ESSID must render as
    // text, not inject markup. A field that will not fit its (worst-case-sized) escape buffer is a bug.
    char essidEsc[kCrackedEssidCap * 6];
    char pskEsc[kCrackedPasswordCap * 6];
    if (!escapeHtml(result.essid, kCrackedEssidCap, essidEsc, sizeof(essidEsc)) ||
        !escapeHtml(result.password, kCrackedPasswordCap, pskEsc, sizeof(pskEsc))) {
        out[0] = '\0';
        return 0;
    }

    // Renders the recovered plaintext PSK into the page — a deliberate, recorded exception to the
    // no-plaintext-PSK doctrine (§4 #14/#19/#20), NOT an oversight (ADR-0039 decision 5): this is a
    // local results viewer on the operator's own hardened SoftAP, reached only by a physical BOOT press.
    // Do NOT "harden" this by redacting the password — that defeats the entire feature (stele:ADR-0012).
    const int written = std::snprintf(out, outSize, "<tr><td>%s</td><td><code>%s</code></td></tr>",
                                      essidEsc, pskEsc);
    if (written < 0 || static_cast<size_t>(written) >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

// Closes the results table, then the two reboot-scoped controls (ADR-0043 §4 #23). Resume is a POST
// *form* (not an <a> link) so a prefetch/OS-captive-check GET cannot reboot the device (decision 3);
// re-provision is a plain GET link to the setup form (a read — safe to prefetch). Both actions the
// server honours only after the response flushes, by rebooting into Station.
const char kDashboardControls[] =
    "</table>"
    "<hr>"
    "<form method='POST' action='/resume' style='margin:.6rem 0'>"
    "<button type='submit'>Resume &amp; hunt</button>"
    "</form>"
    "<p><a href='/config'>Re-provision / re-arm&hellip;</a></p>";

const char kDashboardFoot[] = "</body></html>";

bool maintenanceBackstopDue(uint32_t lastActivityMs, uint32_t nowMs) {
    // Unsigned wrap-safe: the elapsed interval is always < the 30-min bound << the ~49-day millis wrap.
    return static_cast<uint32_t>(nowMs - lastActivityMs) >= kMaintenanceBackstopMs;
}

}  // namespace sapper
