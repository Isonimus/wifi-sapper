---
id: '0119'
title: "HUD glanceable redesign: four bands, RSSI, PMKID badge, armed-TX indicator, lifetime cracked count"
type: slice
status: accepted
date: 2026-11-12
supersedes: []
superseded_by: []
---

## Goal

Land the full ADR-0116 HUD information-hierarchy redesign (everything beyond the
slice-0117 stage-1 collision fix) across both device renderers, and with it the
ADR-0118 lifetime-cracked-count decision, now that the operator has every panel on hand
to re-prove the photographic HUD (§4 #22/#38/#39).

This closes two LEDGER items: the `[feature] HUD glanceable redesign (ADR-0116)` and,
subsumed by its on-hardware re-proof, the `[audit] HUD collision fix (slice-0117)
eyes-on proof` (the collision-proof own-row split is re-verified as part of this
redesign on all three tiers). It does **not** close the deferred
`[feature] HUD pending-upload-queue indicator` (ADR-0116 decision 7 / §4 #13 needs a new
queue-depth pull seam, out of scope here) nor the `[feature]` lifetime *submitted* count
(ADR-0118 keeps that `up:` extension deferred).

What ships:

- **Four persistent bands** (ADR-0116 decision 1): Header (wordmark + version + battery +
  armed-TX chips), Status (phase word + heartbeat + RSSI bars), Body (the SCAN line or
  the capture target, the only band that swaps by `HuntPhase`), Footer (the reference
  counters + sync, moved to the bottom so the live body is the visual centre).
- **B / M1 / M2 / M3 / M4 indicators** (already relabelled in slice-0117) with a **PMKID
  badge set apart** from the row, lit green only on the clientless path (decision 4).
- **A bucketed target RSSI** shown as a small multi-bar glyph in the Status band
  (decision 5), threaded engine -> `HuntSnapshot` -> `ScreenView` as a coarse bucket.
- **Armed-TX chips** in the Header: a red `D` when deauth is armed, an amber `P` when
  PMKID solicitation is armed, shown only when armed (decision 6). The CAPTURED/CRACKED
  banner is constrained to the Body region so it can never cover these safety chips or
  the Status band.
- **A lifetime cracked total** in the Footer (ADR-0118): `cracked:` now reads the
  persisted `CrackedManifest` count through a new `CrackedCountSource` seam, so it no
  longer resets on reboot.

## Definition of Done

- **Given** a compact-tier panel (Cardputer or M5StickC Plus2) capturing a hidden-SSID
  target, **when** the HUD renders, **then** the 17-char BSSID occupies its own
  full-width row and the `B M1 M2 M3 M4` indicator row sits on its own row below it with
  no overlap, on every tier (the slice-0117 collision-proof split, re-proven here).
- **Given** a capturing target whose discovery RSSI is strong, **when** the HUD renders,
  **then** the Status band shows a multi-bar signal glyph filled to the strong bucket,
  and **when** the raw dBm dithers within one bucket between samples, **then** no redraw
  is forced (the bucket is what `ScreenView::operator==` compares).
- **Given** a capture that holds a beacon + a PMKID-bearing M1 (the clientless path),
  **when** the HUD renders, **then** a PMKID badge lights green set apart from the
  B/M1..M4 row; **given** an ordinary four-way capture (no PMKID KDE), **then** the badge
  stays unlit and PMKID is never shown as a sixth slot in the row.
- **Given** a device provisioned with deauth armed and PMKID solicitation disarmed,
  **when** the HUD renders, **then** a red `D` chip shows in the Header and no `P` chip;
  **given** a CAPTURED or CRACKED banner then fires, **then** the banner overlays the
  Body region only and the `D` chip and Status band remain visible (decision 6).
- **Given** a manifest holding N cracked networks and a NewPassword fact that neither
  adds nor removes an entry (a `Changed` re-crack), **when** the HUD renders, **then**
  the Footer shows `cracked: N` (the manifest count, not a drifting bus tally), and
  **given** a reboot, **then** it still shows the persisted count, not zero.
- **Given** any of the above changes, **when** the native suite and all device envs
  build, **then** both renderers compile, the `ScreenView` equality covers every new
  field, and the pure policy tests pass.

## Design

Two layers: a pure data/policy layer asserted on the native lane, and the two device
renderers re-tuned on the panels (photographic, §4 #22/#38/#39).

**Pure layer (host-tested).**

- `net/hunt_snapshot.h` gains `enum class RssiLevel { Unknown, Weak, Fair, Good, Strong }`
  and a pure `rssiBucket(int8_t dBm) -> RssiLevel` (candidate thresholds -55/-67/-78 dBm,
  tunable; never returns Unknown, which the engine assigns for the `kRssiUnset` sentinel
  so the header stays free of the `ap_registry.h` include). `HuntSnapshot` gains
  `RssiLevel rssi` and `bool hasPmkid`.
- `HuntEngine` records `capturingRssi_` at retarget (beside the existing `capturingSsid_`,
  the same app-task-at-retarget write the snapshot's torn-read reasoning already covers,
  §4 #18), and in `huntSnapshot()` sets `snap.rssi` (bucketed, Unknown for `kRssiUnset`)
  and `snap.hasPmkid = collector_.isPmkidValid()` (the ADR-0110 predicate, computed off
  the held M1 via the pure `extractPmkidFromEapol`, so §4 #8/#47 hold; no new extraction
  site). Considered reading `registry_.at(targetIndex_)` in the snapshot instead and
  rejected: `targetIndex_` advances past the selected target, and a registry read in the
  lock-free snapshot would widen the torn-read surface; the single `capturingRssi_` datum
  is the minimal, already-safe value.
- `surface/screen_view.h` gains `rssiLevel`, `hasPmkid`, `armedDeauth`, `armedSolicit`,
  `crackedTotal`, each added to `operator==`; the run-scoped `cracks` field is removed
  (ADR-0118: a lifetime-only footer figure, so a second run tally would be dead state).
- `net/cracked_count_source.h` is a new one-method pull seam (`crackedTotal()`);
  `CrackedManifest` implements it over its existing `size()` (§4 #12, no new parse).
  `ScreenToastSurface` gains `setArmedTx(deauth, solicit)` and
  `setCrackedCountSource(...)`, folds them into the view in `render()`, and drops the
  `cracks_` tally and its `++cracks_` in the `NewPassword` handler (the fact still arms
  the banner). Reused the `setHuntSource`/`setBatterySource` post-construction injection
  shape rather than growing the constructor.

**Device renderers (both, photo-tuned, §3 rule-of-three keeps them two — ADR-0086).**

- `screen_renderer_esp32.cpp` (compact 240x135 + roomy 320x170): reorder to the four
  bands with the Footer at the bottom; add the armed-TX chips to the Header (right of the
  battery), the RSSI bar glyph to the Status band, the PMKID badge to the capturing Body
  set apart from the indicator row, and constrain the banner to a Body-region rect (a new
  `bodyTop`/`bodyBottom` in `Layout`) so it never paints over the Header/Status bands.
  The exact coordinates are candidates (ADR-0002 §5), guarded by the existing off-panel
  `static_assert`s.
- `direct_panel_screen_renderer.cpp` (480x320): the same additions under region-repaint;
  the banner already has its own below-HUD strip (ADR-0086 decision 3) so decision 6
  holds there already. New fields repaint only on change; the Footer moves above the
  banner strip. Each new drawn field clears its own rect first (no whole-panel clear).

`hunt_loop.cpp` wires `setArmedTx(g_deauthArmed, g_solicitArmed)` and
`setCrackedCountSource(*g_manifest)` beside the existing `setHuntSource`.

## Verification

- `test/test_hunt_engine` — `rssiBucket` maps dBm to the four buckets at its boundaries;
  the snapshot carries the bucketed RSSI (Unknown for an unset target) and `hasPmkid`
  off a PMKID-bearing M1 but not off a plain four-way (§4 #8 preserved).
- `test/test_screen_toast_surface` — a fake `HuntSnapshotSource` drives `rssiLevel`,
  `hasPmkid` into the view; `setArmedTx` drives `armedDeauth`/`armedSolicit`; a fake
  `CrackedCountSource` drives `crackedTotal` (and an unwired source resolves to 0); the
  RSSI bucket and the cracked total are edge-compared, so a steady bucket / steady total
  forces no redraw (the ADR-0025 decision 2 render-on-change contract holds for every new
  field).
- `test/test_cracked_manifest` — `crackedTotal()` equals `size()` across inserts and the
  256-cap eviction.
- `scripts/0034-hunt-hud-verify.mjs` — the existing error-gate half (`[SCREEN] enabled`,
  no `[FATAL]`) stays green on a hooks build.
- **Photographic (§4 #22/#38/#39), operator hardware:** eyes-on the Cardputer, M5StickC
  Plus2 (sprite boards), and CYD (direct panel) that the four bands render, the collision
  split holds, the RSSI bars / PMKID badge / armed-TX chips read correctly, the banner
  covers only the Body (the `D`/`P` chips stay visible under it), and the Footer shows the
  persisted lifetime cracked count. Tracked as a LEDGER `[audit]` line.

## As built

Shipped as designed. The pure layer: `net/hunt_snapshot.h` gained `RssiLevel` + the pure
`rssiBucket()` (thresholds -55/-67/-78 dBm) and the `rssi`/`hasPmkid` snapshot fields;
`HuntEngine` records `capturingRssi_` at retarget and sets `snap.rssi` (Unknown for the
`kRssiUnset` sentinel) + `snap.hasPmkid = collector_.isPmkidValid()` in `huntSnapshot()`;
`ScreenView` gained `rssiLevel`/`hasPmkid`/`armedDeauth`/`armedSolicit`/`crackedTotal` (all
in `operator==`) and lost `cracks`; a new `net/cracked_count_source.h` seam is implemented
by `CrackedManifest::crackedTotal()` over `size()`; `ScreenToastSurface` gained
`setArmedTx`/`setCrackedCountSource` and folds them in `render()` (the `NewPassword` tally
removed, it now only arms the banner). `hunt_loop.cpp` wires both new seams beside
`setHuntSource`.

Both renderers were re-laid-out to the four bands with the footer at the bottom, the
banner constrained to `[bodyTop, bodyBottom)` (sprite) / its own below-HUD strip (direct),
the armed-TX chips in the header, the RSSI bar glyph in the status band, and the PMKID
badge set apart at the right of the indicator row. The direct renderer's `drawStatus`
word-clear was bounded to stop short of the RSSI zone (a region-repaint interaction the
sprite renderer's full-frame redraw does not have). Off-panel layout budgets are guarded by
`static_assert`s on both renderers.

Definition of Done: all six scenarios met in the pure/compile layers. The collision-proof
own-row split, the bucketed RSSI with dither-forces-no-redraw, the PMKID-vs-four-way
distinction, the armed-TX-rides-the-view behaviour, and the lifetime-cracked-not-a-tally
counter are host-tested (test_hunt_engine +3 cases incl. `rssiBucket` boundaries,
test_screen_toast_surface +5 cases, test_cracked_manifest +1). Verification: native
503/503, `npm run lint` 0/0, `npm run test:node` green; `cardputer` (compact), `t_embed`
(roomy), and `cyd_3248s035c` (direct panel) all compile SUCCESS, so both renderers and every
layout `static_assert` hold. The photographic eyes-on proof across the three tiers is the
operator's, tracked as the LEDGER `[audit]` line (§4 #22/#38/#39).
