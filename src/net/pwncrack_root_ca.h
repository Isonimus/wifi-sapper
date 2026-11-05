/**
 * @file pwncrack_root_ca.h
 * @brief The pinned trust anchor for pwncrack uploads (ADR-0109): the self-signed GTS Root R4, shared
 *        with wpa-sec.
 *
 * pwncrack.org is fronted by Cloudflare, whose edge presented a Google Trust Services chain on 2026-11-05
 * (ADR-0109 amendment, probe leg headers): leaf -> GTS WE1 -> GTS Root R4, the identical self-signed root
 * the wpa-sec uploader already pins (wpasec_root_ca.h, ADR-0017 decision #1). So this is a thin alias, not a
 * second copy of the PEM: one certificate literal, updated in one place on rotation (rule of three: at this
 * second use, reuse beats a duplicate maintenance hazard here because the two must stay the same byte-string).
 *
 * The device-leg trust is **not yet confirmed on air** (the workstation probe validated the chain, but the
 * device is a different TLS client, and Cloudflare may serve a different edge cert per region/zone): the
 * LEDGER tracks the on-air device leg behind Cloudflare. If the device handshake fails loud against this
 * anchor, the fix is to re-measure the device-presented chain and point this alias at the correct root
 * (never a silent fallback, quality bar §3). Device-only: consumed by uploader_pwncrack.cpp.
 */
#pragma once

#include "net/wpasec_root_ca.h"

namespace sapper {

/// Self-signed GTS Root R4, PEM. The one certificate the pwncrack uploader trusts; the same literal the
/// wpa-sec uploader pins, because pwncrack's Cloudflare edge chains to the same root (measured 2026-11-05).
inline constexpr const char* kPwncrackRootCaPem = kWpaSecRootCaPem;

}  // namespace sapper
