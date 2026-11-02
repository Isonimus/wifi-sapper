/**
 * @file hc22000.h
 * @brief Pure hashcat WPA*01 (.hc22000) line synthesis from a captured handshake (ADR-0108).
 *
 * pwncrack.org takes a hashcat `.hc22000` hashline, not a raw `.pcap` (wpa-sec takes the pcap and runs
 * `hcxpcapngtool` server-side, ADR-0017). So a pwncrack feed forces the device to build the hashline that
 * wpa-sec's server builds for us. This is that build, kept **pure**: it reads the whole captured frames
 * the collector already keeps (ADR-0009 decision #3 parses nothing on-device) and extracts the nonces,
 * MIC and addresses from them *here*, at build time, through the existing `locateEapol` seam — so all
 * 802.11/EAPOL interpretation stays in the host-tested capture core (§4 #8) and the collector is untouched.
 *
 * Only the WPA*01 4-way line is synthesised (ADR-0108 decision #3); PMKID (WPA*02) is not kept by the
 * collector and is deferred. No radio, no filesystem, no `Serial`; host-unit-tested (ADR-0004 lane 1).
 */
#pragma once

#include <cstddef>

#include "net/handshake_collector.h"

namespace sapper {

/// Caller buffer size that covers the true worst case, so a valid in-band capture never fails for want
/// of room: WPA*01*<mic:32>*<ap:12>*<sta:12>*<essid:<=64>*<anonce:64>*<eapol:<=2*kMaxFrameLen=1024>*00
/// plus separators and the NUL totals <= 1224 bytes (a max-size kMaxFrameLen EAPOL). 1280 leaves margin.
/// (Sized from the worst case rather than the common ~250-byte line because a large-key-data M2 — e.g.
/// 802.11r/enterprise — is a capture the collector stores and must still produce a line, not silently fail.)
constexpr size_t kHc22000LineMax = 1280;

/**
 * @brief Build a hashcat WPA*01 (.hc22000) line from a captured 4-way handshake.
 *
 * Writes a single NUL-terminated line `WPA*01*MIC*MAC_AP*MAC_STA*ESSID*ANONCE*EAPOL*00` into @p out,
 * where EAPOL is the M2 payload with its MIC field zeroed (hashcat recomputes it) and ANONCE is taken
 * from **M1** so the pair is replay-counter-consistent (messagepair 00; ADR-0108 decision #2).
 *
 * Returns false, writing nothing usable, when the line cannot be built correctly: M1 or M2 is missing,
 * the ESSID is empty (a hidden network has no crackable WPA*01 line; ADR-0108 decision #4), an EAPOL
 * payload cannot be located or is too short to hold the MIC, or @p out is too small for the full line.
 * A partial line is never emitted — a truncated hashline is a silently wrong upload (quality bar §3).
 *
 * @param hs     the captured handshake (whole frames; nothing is mutated).
 * @param out    destination buffer (use at least kHc22000LineMax).
 * @param outLen size of @p out in bytes.
 */
bool buildHc22000Line(const CapturedHandshake& hs, char* out, size_t outLen);

}  // namespace sapper
