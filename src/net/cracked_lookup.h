/**
 * @file cracked_lookup.h
 * @brief The read-only "is this BSSID already cracked?" seam (ADR-0059).
 *
 * A narrow abstract question, not the whole manifest. Two consumers depend on it — the
 * UploadSupervisor (gate A: drop an already-recovered capture instead of re-uploading it to
 * wpa-sec) and, later, HuntEngine target selection (gate B) — so each couples to the *question*
 * rather than to CrackedManifest's 28 KB concrete type, and each stays host-testable against a
 * trivial fake. CrackedManifest implements it over its existing find() read path (§4 invariant
 * #12), so the answer never comes from a parallel manifest parse.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// Answers "have we already recovered this BSSID's PSK?" — manifest membership, read only.
/// CrackedManifest implements it as `find(bssid) != nullptr`.
struct CrackedLookup {
    virtual ~CrackedLookup() = default;
    virtual bool isCracked(const uint8_t bssid[6]) const = 0;
};

}  // namespace sapper
