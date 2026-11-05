/**
 * @file pcap_reader.h
 * @brief Re-derive a CapturedHandshake from serialized pcap bytes (ADR-0109 decision 2, §4 #46a).
 *
 * The read-side inverse of net/pcap.h's serializeHandshake. The pwncrack backend needs a hashcat
 * `.hc22000` line (ADR-0108 buildHc22000Line), which is built from the whole captured frames; but the
 * retry queue persists only the serialized *pcap*, not the live CapturedHandshake (CaptureQueue/
 * CaptureStore). So at drain time the hashline is re-derived from the one artifact that *is* persisted:
 * parse the pcap back into a CapturedHandshake, then feed it to buildHc22000Line unchanged (one hashline
 * builder, one hashline test, DRY).
 *
 * Input contract: the bytes are this firmware's own serializeHandshake output, read back from the
 * persisted CaptureStore pcap: always one target's single-BSSID frame set, never an attacker-controlled
 * or multi-BSSID file. So the reader reconstructs identity from the beacon and does not re-filter frames by
 * BSSID the way the collector does at capture time; a cross-BSSID or degenerate-beacon pcap is not
 * reachable on this path, so validating against one would be guarding an input that cannot occur.
 *
 * This adds **no new 802.11/EAPOL interpretation** (§4 #8): it composes the existing pure readers
 * (isBeacon, frameBssid, beaconSsid, beaconChannel, locateEapol, identifyMessage) to classify each pcap
 * record back into the handshake, exactly the readers the collector and hc22000 already use. It is the
 * writer's mirror, so it is guarded against drift by a serialize-then-parse round-trip test. Pure: no
 * radio, no filesystem, no `Serial`; host-unit-tested (ADR-0004 lane 1).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/handshake_collector.h"

namespace sapper {

/**
 * @brief Parse serialized handshake pcap bytes back into a CapturedHandshake.
 *
 * Validates the pcap global header (classic microsecond magic, LINKTYPE_IEEE802_11), walks each record,
 * and classifies its frame with the pure capture readers: the beacon populates the BSSID/SSID/channel and
 * beacon frame, each recognised EAPOL message populates its M1-M4 slot. A record whose body overruns the
 * buffer, or claims a frame longer than kMaxFrameLen, is treated as corruption and fails loud.
 *
 * Returns true, and fills @p out, only when the bytes are a structurally sound pcap that reconstructs an
 * `isWpaSecValid()` handshake (beacon + M1 + M2). On any failure, truncation, wrong magic/link type, or a
 * reconstructed set too incomplete to upload, @p out is reset to empty and the call returns false, so a
 * caller never feeds a partial handshake to buildHc22000Line (ADR-0109 decision 2, quality bar §3).
 *
 * @param bytes serialized pcap (as serializeHandshake wrote it); may be nullptr (returns false).
 * @param len   length of @p bytes in bytes.
 * @param out   destination handshake; valid only when the call returns true.
 */
bool parseHandshakePcap(const uint8_t* bytes, size_t len, CapturedHandshake& out);

}  // namespace sapper
