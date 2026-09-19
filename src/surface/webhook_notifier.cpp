/**
 * @file webhook_notifier.cpp
 * @brief Implementation of the pure push-notification surface (ADR-0023).
 */
#include "surface/webhook_notifier.h"

#include <cstdio>
#include <cstring>

namespace sapper {
namespace {

constexpr const char* kNtfyTitle = "WiFi Sapper: cracked";
constexpr const char* kJsonContentType = "application/json";

/// Append one raw byte to @p buf at @p pos if it fits (leaving room for a later NUL), advancing @p pos.
void appendChar(char* buf, size_t cap, size_t& pos, char c) {
    if (pos + 1 < cap) buf[pos++] = c;
}

/// Append @p s verbatim (no escaping) to @p buf at @p pos, bounded by @p cap.
void appendRaw(char* buf, size_t cap, size_t& pos, const char* s) {
    for (; *s != '\0'; ++s) appendChar(buf, cap, pos, *s);
}

/// Append @p s JSON-string-escaped so the surrounding "…" stays well-formed: a quote and a backslash
/// are escaped, and any control byte (< 0x20) is replaced with a space rather than emitted raw, which
/// would break the JSON (Scenario G). High bytes are already UTF-8-clean by the time this runs (the
/// ESSID is sanitized at enqueue, see sanitizeToUtf8), so this only handles the structural cases.
void appendJsonEscaped(char* buf, size_t cap, size_t& pos, const char* s) {
    for (; *s != '\0'; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            appendChar(buf, cap, pos, '\\');
            appendChar(buf, cap, pos, static_cast<char>(c));
        } else if (c < 0x20) {
            appendChar(buf, cap, pos, ' ');
        } else {
            appendChar(buf, cap, pos, static_cast<char>(c));
        }
    }
}

/// Copy @p src into @p dst (bounded, always NUL-terminated), replacing control bytes and any byte that
/// is not part of a well-formed UTF-8 sequence with '?'. An 802.11 SSID is arbitrary octets, but a
/// webhook body must be valid UTF-8: Discord rejects a non-UTF-8 JSON body with 400, which — because a
/// failed send is kept for retry — would occupy a queue slot forever and, once enough such entries
/// accumulate, silently drop every genuinely new crack's alert (the one thing this surface delivers).
/// Neutralising it once here, at enqueue, protects both targets while preserving a legitimately
/// international (valid UTF-8) SSID intact. The continuation-byte scan stops at the terminator, so it
/// never reads past @p src.
void sanitizeToUtf8(char* dst, size_t dstCap, const char* src) {
    size_t di = 0;
    const auto put = [&](char c) {
        if (di + 1 < dstCap) dst[di++] = c;
    };
    for (size_t i = 0; src[i] != '\0';) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (c < 0x20) {  // control byte — never valid in a display name / JSON string.
            put('?');
            ++i;
            continue;
        }
        if (c < 0x80) {  // plain ASCII (includes " and \, which appendJsonEscaped handles).
            put(static_cast<char>(c));
            ++i;
            continue;
        }
        // A UTF-8 lead byte: determine how many continuation bytes it needs.
        size_t need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else {  // a bare continuation byte or an invalid lead — not a valid sequence start.
            put('?');
            ++i;
            continue;
        }
        // Each continuation must be 0x80..0xBF; a NUL (or any other byte) breaks the sequence and stops
        // the scan before it can read past the terminator.
        bool valid = true;
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(src[i + k]);
            if (cc == 0x00 || (cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            put('?');
            ++i;
            continue;
        }
        put(static_cast<char>(c));  // emit the whole valid sequence verbatim.
        for (size_t k = 1; k <= need; ++k) put(src[i + k]);
        i += need + 1;
    }
    dst[di] = '\0';  // di < dstCap always holds (put() leaves room for the terminator).
}

/// True iff @p url's host (authority) is exactly @p domain or a subdomain of it — matching the host,
/// not the path/topic, so an ntfy topic such as ".../discord.com-alerts" is not misread as Discord.
bool hostIs(const char* url, const char* domain) {
    const char* afterScheme = std::strstr(url, "://");
    const char* host = afterScheme != nullptr ? afterScheme + 3 : url;
    size_t hostLen = 0;  // the authority ends at the first '/', ':', '?' or the string end.
    while (host[hostLen] != '\0' && host[hostLen] != '/' && host[hostLen] != ':' &&
           host[hostLen] != '?') {
        ++hostLen;
    }
    const size_t dLen = std::strlen(domain);
    if (hostLen < dLen) return false;
    const char* tail = host + (hostLen - dLen);
    if (std::strncmp(tail, domain, dLen) != 0) return false;
    // Whole host ("discord.com") or a dot-delimited subdomain ("canary.discord.com"), never a mere
    // suffix ("notdiscord.com").
    return hostLen == dLen || *(tail - 1) == '.';
}

}  // namespace

WebhookTarget detectWebhookTarget(const char* url) {
    if (url == nullptr) return WebhookTarget::Ntfy;
    // Match the host Discord serves webhooks from (or a subdomain of it); everything else (ntfy.sh, a
    // self-hosted domain) is ntfy. Host-scoped, not a whole-URL substring, so a ntfy topic that merely
    // contains "discord.com" in its path is not misclassified.
    if (hostIs(url, "discord.com") || hostIs(url, "discordapp.com")) return WebhookTarget::Discord;
    return WebhookTarget::Ntfy;
}

void WebhookNotifier::onAppEvent(const AppEvent& event) {
    // Only a genuinely new/changed crack is a push. Every other fact is the LED's or the serial log's
    // concern; a first-sync backlog is seeded silently (ADR-0019 #5) and raises no notification.
    if (event.type != AppEventType::NewPassword) return;

    if (pendingCount_ >= kMaxPendingNotifications) {
        ++droppedCount_;  // fail loud (accounted), never write past the fixed queue (§3, Scenario E).
        return;
    }
    WebhookNotification& n = pending_[pendingCount_++];
    // Copy the ESSID and BSSID only — never event.password->password (there is no field for it). The
    // ESSID is sanitized to valid UTF-8 here so a non-UTF-8 SSID cannot produce a body Discord rejects
    // (which would wedge the retry queue). Sanitizing at enqueue cleans it once for both targets.
    sanitizeToUtf8(n.essid, sizeof(n.essid), event.password->essid);
    std::memcpy(n.bssid, event.password->bssid, sizeof(n.bssid));
}

void WebhookNotifier::flushInWindow() {
    // Try every pending notification once; compact the ones that fail to the front so they retry on the
    // next window, and drop the ones that succeed (ADR-0023 decision 2; Scenarios C, D).
    size_t kept = 0;
    for (size_t i = 0; i < pendingCount_; ++i) {
        if (send(pending_[i])) {
            ++sentCount_;
        } else {
            pending_[kept++] = pending_[i];
        }
    }
    pendingCount_ = kept;
}

void WebhookNotifier::formatBssid(const uint8_t bssid[6], char (&out)[18]) {
    std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2],
                  bssid[3], bssid[4], bssid[5]);
}

bool WebhookNotifier::send(const WebhookNotification& n) {
    char bssid[18];
    formatBssid(n.bssid, bssid);

    WebhookRequest request;
    request.url = url_;

    if (target_ == WebhookTarget::Discord) {
        // {"content":"**WiFi Sapper: cracked**\n<essid> (<bssid>) - password recovered"} — \n is the
        // two JSON bytes backslash-n. The ESSID is escaped so a quote/backslash in it cannot break out.
        size_t pos = 0;
        appendRaw(bodyBuffer_, sizeof(bodyBuffer_), pos, "{\"content\":\"**");
        appendRaw(bodyBuffer_, sizeof(bodyBuffer_), pos, kNtfyTitle);
        appendRaw(bodyBuffer_, sizeof(bodyBuffer_), pos, "**\\n");
        appendJsonEscaped(bodyBuffer_, sizeof(bodyBuffer_), pos, n.essid);
        appendRaw(bodyBuffer_, sizeof(bodyBuffer_), pos, " (");
        appendRaw(bodyBuffer_, sizeof(bodyBuffer_), pos, bssid);
        appendRaw(bodyBuffer_, sizeof(bodyBuffer_), pos, ") - password recovered\"}");
        bodyBuffer_[pos] = '\0';
        request.contentType = kJsonContentType;
    } else {
        // ntfy: a plain-text body with the title in a header (set by the transport).
        std::snprintf(bodyBuffer_, sizeof(bodyBuffer_), "%s (%s)\npassword recovered - read on device",
                      n.essid, bssid);
        request.title = kNtfyTitle;
    }
    request.body = bodyBuffer_;
    return transport_.post(request);
}

}  // namespace sapper
