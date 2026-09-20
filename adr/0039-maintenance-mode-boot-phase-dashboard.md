---
id: '0039'
title: "Maintenance mode — a boot-phase web dashboard over a hardened SoftAP"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

The web dashboard is the last unbuilt slice-7 surface. `LEDGER.md` and ADR-0021 both frame
it as *"an additional bus subscriber touching no engine code"* — one more `EventSink`
alongside the LED, screen, and webhook. Building it shows that framing is wrong, for a
hardware reason the reactive surfaces do not hit.

**One radio cannot hunt and serve at once.** The hunt loop runs the Wi-Fi radio in
**promiscuous mode, channel-hopping 1/6/11** (`hunt_loop.cpp`, `[HUNT] discovering
channels=1,6,11`). Everything that needs network reachability — uploads, the hourly sync,
the push webhook — happens inside the brief **STA windows** the `UploadSupervisor` opens and
closes (ADR-0023). A served HTTP dashboard is a different kind of thing from the other
surfaces: the LED/screen/webhook are *reactive emitters* that consume a fact and push it out,
whereas a dashboard is a **server** that must accept an inbound HTTP connection whenever the
operator opens a browser — i.e. it needs a *continuous, single-channel, managed* network
presence. That is mutually exclusive with promiscuous channel-hopping. Served over STA as
"one more sink," the dashboard would be reachable only during the few-seconds-per-hour
windows — a server that exists for seconds an hour is not a dashboard.

So the dashboard **forces a deliberate hunt-suspended state**: the radio must stop hunting
and come up as a stable AP for the dashboard to be reachable at all. That state is the whole
decision this ADR settles.

Three further forces constrain its shape:

- **Headless-first (ADR-0001).** Entry into the suspended state must work on a board with no
  screen and no keyboard. The one control nearly every ESP32 board has is the BOOT/GPIO0
  button; `main.cpp:138` already carries an inert `reprovisionRequested()` seam (`return
  false;`) carved for exactly an operator "re-enter setup" signal.
- **The dashboard's purpose is to show recovered results — which are plaintext PSKs.** This
  runs into the no-plaintext-PSK doctrine (§4 #14/#19/#20), but under a *different* threat
  model than those invariants address (see decision 5).
- **The suspended-state AP is wireless, and the portal passphrase is published.** The
  first-boot portal uses `kSoftApPassword = "sapper-setup"`, documented in `README.md`. If
  the dashboard reused it, then although *entering* maintenance needs a physical BOOT-hold,
  once the AP is up **anyone in radio range who read the README could join and read every
  recovered password** — a wireless audience far broader than the person who pressed the
  button. This over-exposure is new to this feature.

## Decision

**Maintenance is a third boot phase, not a live runtime mode.**

1. **Boot-phase model.** `main.cpp` is already a boot state machine (Provisioning / Station,
   chosen by `decideBootPhase()`). Maintenance becomes a third phase. It is entered from a
   clean boot, raises the SoftAP, serves the dashboard + config, and leaves only by rebooting
   into Station. The alternative — pausing a *live* hunt in place, tearing down promiscuous
   mode, raising the AP while keeping RAM state, and resuming without a reboot — was rejected:
   it is a large amount of radio-lifecycle complexity under a running engine, and the quality
   bar (§3) requires complexity to earn its place. The boot-phase model never reconfigures the
   radio live; the AP comes up clean exactly as the captive portal already does (ADR-0006).
   **The honest cost:** the dashboard shows *persisted* state, not live session counters,
   because entering via reboot wipes RAM. That is acceptable — arguably ideal: the persisted
   data (the recovered passwords) is precisely what the operator opens the dashboard to read.

2. **Entry: hold BOOT at power-on.** A HAL GPIO seam reads the BOOT/GPIO0 button at startup
   and drives the existing `reprovisionRequested()` signal (renamed to a maintenance signal).
   `decideBootPhase()` gains this input with this precedence, all pure and host-tested:
   - unprovisioned (no usable stored triad) → **Provisioning**, regardless of the button —
     there is nothing to maintain and no results to show;
   - provisioned **and** the maintenance signal is asserted → **Maintenance**;
   - provisioned, no signal, `staFailCount < kStaRetryBudget` → **Station** (the normal hunt);
   - otherwise → **Provisioning** (the STA-fail fallback, ADR-0006 #3, keeps the config form).
   Maintenance is inherently a physical-access operation, which is correct for a results
   viewer that exposes recovered secrets. (Repurposing the button this way means BOOT-hold no longer
   opens the setup portal: re-provisioning a *working* device stays reachable via the STA-fail fallback
   today, and becomes reachable from the config form Maintenance serves in **slice-0041** — decision 8.
   slice-0040 itself serves the read-only dashboard only.)

3. **The dashboard reads persisted state; it is NOT an `EventBus` subscriber.** This revises
   ADR-0021's incidental expectation that the dashboard would be "one more sink." In a
   hunt-suspended phase the engine is not running, so there are *no live facts to subscribe
   to*. The dashboard instead reads the existing pull seams: recovered results through the
   `CrackedManifest`/`CrackedStore` seam (§4 #12), the provisioning record through
   `loadProvisioning()` (§4 #6), and the persisted capture-queue depth. No new store-access
   site is created. ADR-0021's prose is immutable, so this correction to its expectation is
   recorded *here* — the bus itself is unchanged; only the assumption that a *served* surface
   would ride it was wrong.

4. **Rendering is pure and host-tested — streamed in chunks, not one buffer.** A `net/dashboard`
   module (sibling to `net/provisioning_form`, ADR-0037) renders the page from persisted state.
   *Correction to this ADR's first draft (recorded per global §2, evidence over a prior choice):*
   the draft said "one fixed buffer, fail loud if it does not all fit," mirroring `buildSetupForm()`.
   That is wrong for a **data-dependent** page. The setup form is fixed-shape, so overflow there only
   ever means a code bug; the dashboard lists up to `kMaxCrackedEntries` (256) recovered results —
   ~36 KB of HTML. A single fixed buffer would then either need a ~36 KB stack buffer this no-PSRAM
   board cannot spare, or "fail loud if it all does not fit," which breaks the viewer on a *large*
   account — fail-loud on success, not on a bug. So the module exposes `buildDashboardHead(stats,…)`,
   `appendCrackedRow(result,…)`, and a static `kDashboardFoot`; the device serve path streams them
   (`WebServer::sendContent`) — head once, one row per result, then the footer — so the whole page is
   never materialised at once. Each piece still renders into its *own* small bounded buffer where an
   overflow genuinely is a bug (a single row cannot exceed its known max), so the fail-loud-returns-0
   guarantee (§3) stays meaningful. The ESSID and PSK are HTML-escaped (they arrive from the air / an
   external service) so a crafted ESSID cannot inject markup. `DashboardStats` carries only
   **persisted** facts (recovered count, capture-queue depth, deauth-arm, and *ever-synced* from the
   manifest's persisted `freshManifest` flag) — deliberately **not** a "last sync ok / +N new" line:
   that status is a millis()-based RAM value (cracked_store.h) the reboot into Maintenance wipes, so it
   could only read back a dishonest zero. Rows come through the `CrackedManifest` seam via a new
   read-only `entryAt(i)` view (§4 #12 intact — the manifest stays the store's single reader/writer).

5. **The dashboard shows recovered plaintext PSKs — a deliberate, recorded exception to the
   no-plaintext-PSK doctrine (§4 #14/#19/#20), not a silent one (global §2).** Those
   invariants forbid a plaintext PSK in an **off-device payload to a third-party service**
   (ntfy/Discord) — a network egress the operator does not control. The dashboard is the
   opposite: a **local** surface on the operator's **own** SoftAP, reachable only after a
   **physical** BOOT-hold, showing the operator **their own** recovered results — the entire
   reason the appliance exists. A physical attacker who can BOOT-hold the device can already
   dump its flash and read the PSKs at rest (ADR-0006 already warns of this), so withholding
   them from the physically-triggered local viewer buys nothing while defeating the feature.
   Therefore `DashboardModel` **deliberately carries the recovered PSK strings** — unlike the
   secret-free `SetupFormModel` of #20. This is the one site a later contributor would
   plausibly "harden" by redacting; the render site cites this ADR so the choice announces it
   is on purpose (stele:ADR-0012).

6. **The wireless over-exposure gap is closed by a dedicated maintenance AP passphrase.** A
   new optional `ProvisioningRecord.maintenancePass` field: when set, the **Maintenance**
   SoftAP uses it, so only the operator (who set it) can join and read results; when empty, it
   falls back to `kSoftApPassword` — convenience over secrecy, the operator's explicit choice.
   Validation is pure and host-tested: empty **or** ≥ 8 characters (the WPA2-PSK minimum,
   below which `WiFi.softAP()` would reject it — a fail-loud validation, not a silent
   downgrade to open). It round-trips through the #6 seam with a behaviour-preserving absent
   default (empty) for devices provisioned before it existed, exactly like the ADR-0035
   selector defaults. The **first-boot Provisioning** portal keeps `kSoftApPassword` (no
   results exist yet to expose); only Maintenance is hardened. **The operator sets it in the existing
   captive-portal setup form** (a new `maintpass` field), shipped in slice-0040 rather than deferred to
   slice-0041: decision 5 serves plaintext PSKs over this AP, so a device that could not have its
   passphrase set by an operator would ship the very over-exposure this decision closes. The field is a
   secret handled exactly like the wpa-sec key under ADR-0037 (§4 #20) — accepted, `resolveProvisioningUpdate`
   keeps it on a blank re-save, and it is **never** echoed back into served HTML (`SetupFormModel` gains
   only a `hasStoredMaintenancePass` boolean, no string).

7. **Stays until explicit resume, with a safety backstop.** Exit is a reboot into Station,
   reachable three ways: a physical power-cycle *without* BOOT-held (→ Station); the
   in-dashboard resume control (slice-0041); and a **30-minute no-activity auto-resume**
   backstop that reboots into Station so a walked-away unit cannot self-DoS its own
   endless-hunt identity. The backstop protects the product's core promise ("hunts endlessly")
   against the operator's chosen "stay until resumed" behaviour.

8. **Controls mutate only as reboot-scoped intents, never live engine ownership — a light
   extension of ADR-0021 #13 (observers never own the engine).** The dashboard's control
   actions do not reach into a running engine (there is none in Maintenance): *resume* =
   restart into Station; *force sync now* = a persisted flag consumed on the next Station boot
   (it cannot run *in* Maintenance — the radio is the AP); *re-arm deauth* = the existing
   config form, already an NVS field (#6). A surface still never owns the live engine; it
   schedules a reboot-scoped intent that the next Station boot honours. This extends, and does
   not supersede, ADR-0021 #13. (slice-0041.)

9. **Device behaviour a unit test cannot assert ships a verify script (§3).**
   `scripts/00NN-maintenance-verify.mjs` drives the real device: BOOT-hold entry → SoftAP
   raised with the maintenance passphrase → dashboard serves the persisted results → resume
   (or the backstop) returns to Station. The pure halves — `decideBootPhase()` precedence,
   `buildDashboard()` rendering, `maintenancePass` validation, the backstop timer arithmetic —
   are host-tested on the native lane.

10. **Slice sequence.** slice-0040 ships the phase, the BOOT-hold trigger, the hardened
    SoftAP, the read-only dashboard (results incl. PSKs, counts, arm/ever-synced status), the
    Maintenance panel screen, and the backstop — a device usable via physical entry/exit. slice-0041
    adds the in-dashboard controls (decision 8). §4 invariant #21 is added in the slice-0040 commit;
    the controls' invariant lands with slice-0041.

11. **On a board with a screen, Maintenance draws a status panel — connection info + a count only, no
    PSKs.** The panel shows "MAINTENANCE", the SoftAP SSID, the dashboard URL, and the recovered-network
    count, so a screened operator knows how to reach the dashboard without a serial console. It
    deliberately does **not** render the recovered passwords: the panel is *shoulder-surfable* by anyone
    near the device, whereas the dashboard is gated behind the hardened AP passphrase (decision 6). That
    asymmetry — connection info on the open panel, secrets only behind the passphrase — is the point. The
    panel is a direct one-shot draw (like the existing bring-up frame), not the EventBus-driven
    `ScreenView`/renderer path (ADR-0025): there is no engine and no live fact stream in this phase, so
    that view-model machinery does not apply (this is a further reason the rule-of-three extraction does
    not fire — see Consequences). A screenless board's `NullDisplay` makes the draw an inert no-op.

## Consequences

- **Closes the `LEDGER.md` web-dashboard item and corrects its "one more sink" framing.** The
  dashboard is a served surface in a hunt-suspended boot phase reading persisted state, not an
  `EventBus` subscriber — the load-bearing correction of this ADR.
- **Adds §4 standing invariant #21** in the slice-0040 commit: the Maintenance boot phase, the
  persisted-state dashboard, the hardened AP passphrase, and the recorded PSK-display
  exception.
- **Extends ADR-0006** (a third boot phase, a new optional provisioning field, reuse of the
  portal SoftAP/DNS/HTTP infra) and **ADR-0021** (the dashboard is not a sink; controls are
  reboot-scoped intents). Supersedes neither.
- **Deliberately does *not* follow the secret-free-model pattern of #17/#19/#20 for the
  dashboard.** #20's `SetupFormModel` carries no secret by construction; `DashboardModel`
  carries PSKs on purpose (decision 5). The two models sit side by side precisely to make the
  difference — a form that must never echo a secret vs. a results viewer whose job is to show
  them — visible and intentional.
- **The rule-of-three view-model→renderer extraction is *not* triggered.** `LEDGER.md` flagged
  the dashboard as the third surface after the LED and screen, the trigger to consider
  hoisting a shared "surface renders a view-model" abstraction (ADR-0025). It is not: the LED
  and screen compute a view-model that an abstract *device* renderer draws to hardware; the
  dashboard renders persisted state to an HTML string (like `buildSetupForm()`), sharing
  neither the renderer interface nor the live-fact input. The count of "surfaces" coincides;
  the pattern does not. The LEDGER note is updated to say so rather than fire a spurious
  extraction.
- **Given-up cases, recorded in `LEDGER.md`:** live in-session counters and a last-sync-ok/+N line
  are not shown (the reboot-based entry wipes RAM — only persisted state survives, so the dashboard
  shows *ever-synced* from the persisted fresh flag, not the last sync's result); *force sync* cannot
  run inside Maintenance (the radio is the AP), only as a next-boot flag; and there is no runtime
  (non-reboot) entry into Maintenance from a live hunt (decision 1's rejected alternative).
- **One deliberate log-and-continue (not a swallowed error).** If the persisted results store fails to
  load at Maintenance entry, `main.cpp` logs `[FATAL] maintenance: could not load recovered results` on
  serial (the operator's/verify's authoritative channel) and still raises the AP so the operator is not
  stranded, showing the entries that did load (0 on a hard fault). Halting would brick the viewer and
  rebooting would loop; the loud serial line is the fail-loud signal. Recorded here so it is not mistaken
  for a §3 violation.
