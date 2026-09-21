# WiFi Sapper — Project Conventions

The WiFi Sapper is an autonomous, portable ESP-32 firmware appliance spun off from
The Adversary (`../adversary`). It runs WPA/WPA2 handshake auto-hunt **endlessly**; on each
new capture it auto-uploads the handshake to `wpa-sec.stanev.org`, and once an hour it
fetches cracked results back and announces any newly recovered passwords. Unlike its
interactive parent, the Sapper is **headless-first**: the capture→upload→sync engine has no
UI dependency, and every surface (optional display, LED, web dashboard, push webhook) is an
event-bus subscriber — so the same firmware runs on hardware with or without a screen,
keyboard, or SD card.

Stack: C++17 on the Arduino framework over ESP-IDF (pioarduino platform + Bruce's patched
esp32-arduino libs, required for raw deauth TX), built with PlatformIO. LovyanGFX for display
(auto-detect across M5Stack, CYD, Lilygo, and generic panels). ArduinoJson; Unity for native
unit tests. First reference target: **M5Stack Cardputer ADV** (ESP32-S3, no PSRAM — the
Adversary repo labels this same hardware plainly as "Cardputer").

General working practices — quality bar, testing standard, commit hygiene, delegation,
correction, language — live in [`docs/quality-bar.md`](docs/quality-bar.md) and apply here
without being restated. It ships with the method and is vendored into this repo, so it is
yours to adapt; an update keeps what you changed (stele:ADR-0024).

This file carries only what is specific to **this** repo. Restating a rule from the bar here
would create a second copy with no sync path, which is the failure stele:ADR-0005 exists to
prevent.

## 1. Document taxonomy — four kinds

Every document is exactly one of four things.

| Kind | Files | Rule |
|---|---|---|
| **Immutable** | `adr/*.md`, `slices/*.md` | Written once. Body prose is never edited. Only status/supersession fields may change. |
| **Generated** | `adr/INDEX.md` | Built by script from frontmatter. Never hand-edited. |
| **Ledger** | `LEDGER.md` | Exactly one per repo. The only hand-maintained tracker. |
| **Live doc** | `README.md`, `docs/*.md` | Describes how something behaves *now*. Updated **in the same change** as the thing it describes, and cited from this file so it is never orphaned (stele:ADR-0010). |

- **ADR** — a decision later work must obey (a mechanism, data format, or boundary).
  Asserts *"on date X we chose Y because Z"*: a historical claim, true forever.
- **Slice** — one feature work-unit. Written before implementation, **frozen at merge**
  and rewritten to past tense: *"this is what shipped."* Freezing converts it from a
  current-state claim (always going stale) into a historical one (never stale).
- **README.md** — live document; update it in the same change that alters any
  user/dev-facing feature or API. It is the repo's operator/dev entry point — the build and
  verify commands and the first-boot provisioning guide (ADR-0006) — cited here so it is not
  orphaned (stele:ADR-0010).

**Single writer, one direction.** An ADR records a deferral *once*, as a fact about that
decision. `LEDGER.md` cites the ADR. **Never reach back into an ADR to close a ledger
item.** Closing an item means deleting its line from the ledger.

Changing our minds means writing a **new** ADR that supersedes the old one, never editing
it. The superseding note must say *why the old reasoning was wrong* — that record is the
most valuable thing this workflow produces, and an in-place edit destroys it.

A committed document's body may **gain** lines — an appended `## Amendment — <date>: …`, or a
correction marker placed at the claim it corrects — and may never lose or rewrite one. The
hook enforces this (stele:ADR-0019); frontmatter is exempt, because status and supersession
fields are how a record announces it was superseded.

This is also how a justified rule-violation gets recorded. `~/.claude/CLAUDE.md` §2 says a
justified violation is written down as a decision rather than taken as a silent exception;
in this repo, that decision is a new or superseding ADR.

## 2. Enforcement — invariants are executable

`node scripts/lint-docs.mjs` runs from a pre-commit hook and in CI. The hook checks the
**commit**, not the working tree, so a fix you forgot to stage cannot green a red commit
(stele:ADR-0018); alongside the linter it verifies that `adr/INDEX.md` matches the corpus and
that immutable bodies have only gained lines (stele:ADR-0019).

A citation is bare (`ADR-NNNN`) only when it means *this* repo, and qualified
(`<repo>:ADR-NNNN`) otherwise (stele:ADR-0009). The linter resolves citations in `LEDGER.md`,
in the corpus, and in the prose that is read as instruction — this file, `README.md`,
`docs/`, `.claude/commands/` — so a citation that rots there fails the build rather than
quietly misrouting the next session (stele:ADR-0020).

A rule enforced by memory is a rule that holds until the first busy afternoon. If a
convention matters, it gets a rule; if it genuinely can't be checked, say so out loud
rather than writing it down and trusting it.

Run `/wrap-up` before finishing a task.

## 3. Verification harness — measure, don't assume

Some changes cannot be asserted in a unit test: rendering, world generation, physics,
timing, anything whose correctness is "does it look and behave right at runtime". The
answer is **not** to skip verification and eyeball it once by hand.

**Any slice whose behaviour a unit test cannot assert ships a verification script.**

`scripts/<slice>-verify.mjs` — drives the real system headlessly, exercises the specific
behaviour the slice claims, and:

- **fails on any console error or page error** — this half is pass/fail and machine-checkable;
- **writes artifacts** (screenshots, dumps, measured numbers) for human review — this half
  needs eyes, and that is fine, as long as the first half still runs unattended;
- **is named in the slice's `## Verification` section**, so the claim and its evidence are
  linked;
- **is wired into `package.json`**, and its error-check half runs in CI.

That last point is the one that gets skipped, so it is **rule-checked** (R11): the linter
fails if any `scripts/*-verify.mjs` is absent from `package.json`. A verify script that is
not wired runs exactly once, on the day it was written, and is dead thereafter — it
documents that the slice worked once, which is not what a regression check is for. Probes
are exempt: a probe answers its question once and the number lands in an ADR.

Every slice carries two required sections, both rule-checked: `## Verification` names the
proof (R12), and `## Definition of Done` states the acceptance criteria as Given/When/Then
scenarios written before the code (R13, stele:ADR-0011). Each scenario names its proof in
`## Verification`; the linter checks the sections exist and that the Definition of Done
holds a full triad — it cannot check that a scenario is *right*, which is what `/wrap-up`
is for.

Probes are the same tool used before the fact: when a design question has a measurable
answer (how many caves per chunk, what the frame cost is), write `scripts/<topic>-probe.mjs`
and put the **measured numbers** in the ADR. Design decisions cite data, not estimates.

## 4. Standing invariants

Repo-specific definition-of-done rules. A slice is not complete until it satisfies every
one that applies. Each cites the ADR that created it; exceptions are listed with the
reason, so nobody "fixes" a deliberate choice.

These live **here, in the repo** — not in assistant memory. A rule that governs the
codebase must be greppable, diffable, reviewable, and survive a change of machine
(stele:ADR-0005).

Each row declares how it is **enforced**, so an aspiration is never mistaken for a
guarantee:

- `verified_by: <script>` — a wired verify script or lint rule checks it;
- `pending (LEDGER)` — enforceable but not yet enforced; a ledger item carries the debt;
- `review-only` — enforceable only by human judgement, so `/wrap-up` is the enforcement.

The declaration is a convention checked at review, not by the linter — the linter does not
read this file (stele:ADR-0004). What it *does* enforce is that every `verified_by` script is
actually wired (R11), so a row cannot claim a check that runs nowhere.

| # | Invariant | Source | Enforced by |
|---|---|---|---|
| 1 | Serial observation commands (`ping`, `state`, event tap) mutate no engine state — they expose only what the device already publishes or logs. | ADR-0003 | review-only |
| 2 | Injected stimulus enters the engine only through the same seams a real event uses — never a parallel test-only path. | ADR-0003 | review-only |
| 3 | Every state that blocks before the engine loop runs (e.g. first-boot captive portal) pumps the serial channel. | ADR-0003 | verified_by: scripts/0007-provisioning-verify.mjs |
| 4 | The actuation/stimulus vocabulary is absent from any build without `SAPPER_TEST_HOOKS` — no shipped binary can be commanded to deauth or upload over serial. The serial stimulus tokens (`inject-handshake`/`force-sync`/`inject-cracked`/`inject-capture`) live inside `#ifdef SAPPER_TEST_HOOKS` in the pure parser and their gated dispatch (in `serial_channel`) calls the existing `huntLoop` injectors — the same seams a real event uses (#2), never a parallel path. The enforcement (ADR-0047) exploits that the native unit lane compiles the parser *without* the flag, so the shipped-build absence is asserted on every run. | ADR-0003 | verified_by: test/test_serial_command (the no-hooks/shipped parser rejects every stimulus token) + scripts/0048-serial-stimulus-verify.mjs (a hooks build dispatches injection over serial to drive the shipped loop); review-only (gated dispatch wiring) — enforcement built by ADR-0047 |
| 5 | No cloud-CI lane depends on attached hardware — device-in-the-loop verification runs on a hardware lane and its artifact is committed for review. | ADR-0004, ADR-0051 | review-only (the `.github/workflows` jobs run on `ubuntu-latest` only — host build/test, no attached device; ADR-0051); the hardware-runner lane for device-in-the-loop lane-3 artifacts stays pending (LEDGER) |
| 6 | Provisioned secrets (WiFi credentials, wpa-sec key) are read and written only through the `persistProvisioning()` NVS seam — never scattered across call sites. | ADR-0006 | review-only |
| 7 | No build's serial vocabulary can write or clear provisioning credentials — provisioning is never a serial actuation path. | ADR-0006 | review-only |
| 8 | All 802.11/EAPOL frame interpretation lives in the pure capture core, compiled and unit-tested on the native lane; the RF sniffer/TX seams forward raw bytes only and carry no protocol logic. | ADR-0009 | review-only |
| 9 | Capture artifacts (pcap bytes) are emitted only through the `CaptureSink` seam — no module opens or writes a capture file directly. | ADR-0009 | review-only |
| 10 | The promiscuous RX callback forwards raw frame bytes to the pure core and does nothing else — no allocation, blocking, or protocol interpretation in the Wi-Fi-driver callback context. | ADR-0011 | review-only |
| 11 | A consumer is switched under a live `RadioSniffer` only via the atomic router the engine holds as the sniffer's single consumer for its started lifetime; a sink is reset or destroyed only after a quiesce settle with the router aimed away from it — never by a `stop()`/`begin()` re-attach, which cannot hard-join an in-flight callback. | ADR-0015 | review-only |
| 12 | Cracked passwords are read and written only through the `CrackedManifest`/`CrackedStore` seam — no surface parses a results file or the download body directly. Mirrors #9 (captures via `CaptureSink`) and #6 (secrets via the provisioning seam), so slice-7's several surfaces cannot each grow their own store parse. | ADR-0019 | review-only |
| 13 | Surfaces receive engine facts only by subscribing to the `EventBus` (one `EventSink::onAppEvent`); the engine never calls a surface directly, and no surface reaches into engine state except the pull snapshot the on-air verifies use. The capture→queue data path stays a direct `CaptureReadyObserver` seam and is never on the bus (a dropped fact is cosmetic; a dropped capture is a lost handshake). Enacts ADR-0001's observers-never-owners boundary. | ADR-0021 | review-only |
| 14 | A *transmitting* surface (one whose reaction needs the network) observes facts on the `EventBus` like any surface but transmits only inside a supervisor-owned STA window, via the `WindowNotifier` seam the supervisor flushes (`setNotifier`) — never blocking IO inside `onAppEvent` (which would stall the synchronous bus dispatch). It reaches connectivity only through that seam, not by opening its own radio path. The push webhook is the first; a payload it sends off-device carries no plaintext PSK (the notification type has no password field). | ADR-0023 | review-only |
| 15 | Deauth/disassoc frame *construction* lives in the pure `deauth` core, compiled and unit-tested on the native lane; the `RawTransmitter` seam forwards fully-formed frame bytes only and carries no frame-building logic (the TX mirror of #8's read-side purity and #9's raw-bytes RX seam). *(This row's former `SAPPER_TEST_HOOKS` compile-gate clause was superseded by ADR-0029; the gating is now #16.)* | ADR-0027 | review-only |
| 16 | The raw-TX *capability* (the device `esp_wifi_80211_tx` path and the `ieee80211_raw_frame_sanity_check` SDK bypass) ships in every device binary, but a deauth frame is transmitted only when the operator has explicitly *armed* deauth through the provisioning toggle (`ProvisioningRecord.deauthEnabled`, default **off**): `hunt_loop` injects the `RawTransmitter` into the `HuntEngine` only when armed, and the engine transmits (broadcast deauth/disassoc at `capturingBssid()` during Capturing) only when it holds a transmitter — a disarmed or unprovisioned device transmits nothing. Supersedes #15's former `SAPPER_TEST_HOOKS` compile-gate (ADR-0027 decision 3): the gate moved from compile-time absence to a runtime, operator-armed default-off, because the appliance's authorized-industrial purpose needs deauth in the shipped loop while a lost/misused unit must not jam by default. | ADR-0029 | verified_by: scripts/0030-deauth-loop-verify.mjs (shipped-armed TX) + test/test_hunt_engine (disarmed = no TX); review-only (portal/hunt_loop wiring) |
| 17 | A capture *fact* rides the `EventBus` only as `AppEventType::HandshakeCaptured` carrying an identity-only `CaptureFact` payload (bssid + ssid) — never the pcap frame bytes. The capture→queue *data* path stays the must-deliver `CaptureReadyObserver` seam (#13) and pcap bytes stay quarantined to the `CaptureSink` seam (#9); the cosmetic broadcast is *structurally* incapable of carrying capture bytes (the payload struct has no frame member), so both invariants hold by construction, not by review vigilance. The fact is published only after a successful enqueue, so it never announces a capture the queue dropped. | ADR-0031 | verified_by: test/test_upload_supervisor + test/test_event_bus (identity-only payload, published only on successful enqueue) + scripts/0032-capture-notify-verify.mjs (on-air end-to-end); review-only (surface wiring) |
| 18 | Live in-flight hunt state (the current phase/channel/target and which of Beacon/M1–M4 the collector holds) reaches a surface only through the read-only `HuntSnapshotSource::huntSnapshot()` pull seam, read on the app task and best-effort/cosmetic — never the `EventBus`. The bus carries discrete facts a surface may miss without harm; streaming per-frame live state onto its synchronous dispatch would make every `onAppEvent` a hot path. The read takes no lock (the collector is written on the driver task, §4 #10): the `bssid` is written once at retarget (app task), the presence flags are a monotonically-written length field, and the `ssid` (re-written per beacon on the driver task) is published in a single complete-value memcpy so a concurrent read never sees it empty — so a torn read is at worst a one-tick-stale cosmetic blip. Concretises #13's "pull snapshot" for live state; refines, does not supersede, #13. | ADR-0033 | verified_by: test/test_hunt_engine (snapshot reflects engine state) + test/test_screen_toast_surface (surface renders it, no-source fallback) + scripts/0034-hunt-hud-verify.mjs (on-air lit HUD); review-only (surface/engine wiring) |
| 19 | Off-device push is gated *per notification type* by the operator's provisioned enable-set (`notifyCaptured`/`notifyCracked`/`notifySyncError` in `ProvisioningRecord`, NVS-backed like the ADR-0029 deauth arm, mapped to a pure `WebhookNotifyPolicy` the `WebhookNotifier` consults) — and that enable-set gates the *transmitting webhook only*. The local LED/screen/serial surfaces render every fact unconditionally and are never gated by it (a dropped local glance is not why the selector exists; off-device push volume/cost/privacy is). Every pushed payload stays identity-only / secret-free across all three kinds: a capture push carries essid+bssid and, structurally, no pcap frame (it copies the frame-free `CaptureFact` of #17 into a `WebhookNotification` that has no frame member), a sync-error push carries only a static status line, and no kind has a password field — so §4 #14's no-PSK guarantee holds by construction for all of them. Extends #14 (transmit-in-window, no PSK) and builds on #17 (frame-free capture fact); does not supersede either. | ADR-0035 | verified_by: test/test_webhook_notifier (per-type gating, one-per-BSSID dedup, sync-error edge-trigger, per-kind secret-free render) + test/test_provisioning_store (round-trip + behaviour-preserving absent-key defaults) + scripts/0036-webhook-capture-verify.mjs (on-air capture push); review-only (portal/hunt_loop wiring) |
| 20 | The captive-portal setup form is rendered by `buildSetupForm()` from a secret-free `SetupFormModel` (booleans + "a key/webhook is stored" flags — no `ssid`/`pass`/`key`/`webhookUrl` string), so no provisioned secret is ever interpolated into served HTML: the no-echo guarantee holds *by construction* (the renderer is never handed a secret), like the frame-free `CaptureFact` of #17 and the password-field-free `WebhookNotification` of #19. The form renders the stored deauth/notify toggle state back, and a re-save keeps a stored `key`/`webhookUrl` whose field is submitted blank while taking the SSID/passphrase pair and the toggles from the submission (`resolveProvisioningUpdate`), so re-opening the portal (the STA-fail fallback, ADR-0006 #3) neither silently resets the arm/selector toggles nor wipes an un-retyped secret. Both functions live in the pure `net/provisioning_form` module and are wired through the ADR-0006 #6 `loadProvisioning()`/`persistProvisioning()` seam — no new NVS access site. Extends ADR-0006's provisioning boundary and closes the statelessness the ADR-0029/-0023/-0035 toggles each deferred; supersedes none. | ADR-0037 | verified_by: test/test_provisioning_form (checkbox-state render, `required`-drop when a key is stored, no `value=` prefill, buffer-overflow fail-loud, keep-on-blank/replace-on-value merge, first-boot verbatim); review-only (portal `handleRoot`/`handleSave` wiring) |
| 21 | The Maintenance boot phase is a **hunt-suspended** phase entered only from a clean boot — a BOOT/GPIO0 hold (`maintenanceRequested()`, driving the ADR-0006 #3 boot gate via its repurposed third input) on a *provisioned* device — never a live radio reconfiguration, and left only by rebooting into Station (a power-cycle without BOOT, or the 30-min no-activity backstop). Its dashboard reads **persisted** state only — recovered results through the `CrackedManifest`/`CrackedStore` seam (#12, via the read-only `entryAt`), the record through `loadProvisioning()` (#6), the capture depth through the `CaptureStore` — never the `EventBus` (there are no live facts in a suspended phase; corrects ADR-0021's "one more sink" expectation), and is **streamed** by the pure `net/dashboard` functions (`buildDashboardHead`/`appendCrackedRow`/`kDashboardFoot`) so the up-to-256-entry page is never materialised whole. It **deliberately serves recovered plaintext PSKs** — a recorded exception to the no-PSK doctrine of #14/#19/#20 (a local viewer on the operator's own SoftAP behind a physical BOOT-hold, not an off-device payload; ADR-0039 decision 5) — so the Maintenance SoftAP is hardened by an optional `ProvisioningRecord.maintenancePass` (WPA2-length or empty-for-default, set in the setup form accepted-but-never-echoed like the key, #20). The on-device panel shows connection info + a count only, never PSKs (the panel is shoulder-surfable; ADR-0039 decision 11). Extends ADR-0006 and ADR-0021; supersedes neither. | ADR-0039 | verified_by: test/test_provisioning (Maintenance gate + `isUsableMaintenancePass` + label), test/test_dashboard (streamed render incl. PSK + HTML-escape + per-piece fail-loud + backstop), test/test_provisioning_store (`maintenancePass` round-trip + absent default), test/test_provisioning_form (`maintpass` no-echo/keep), test/test_cracked_manifest (`entryAt`); scripts/0040-maintenance-verify.mjs (on-device entry/AP/serve); review-only (`main.cpp` gate wiring, `MaintenancePortal` serve, panel draw) |
| 22 | The shipped boot face renders only the product splash (`drawSplashFrame` — name + `kFirmwareVersion` + tagline, via the `drawText` primitive slice-0026 proved on the panel); the RGB-blocks/border/diagonal **panel-parameter proof** (colour/byte order, offset/clipping, mirror/rotation — ADR-0002 §5) renders only under the `SAPPER_TEST_PANEL` bench probe (`panel_probe`), absent from every shipped build like the other probes (aligns #4). So no shipped unit shows a test pattern at boot, while `verify:device` still dumps the diagnostic from a hooks build — the same proof slice-0005 built, moved off the shipped face. The version shown is the single `kFirmwareVersion` source (no magic string in the boot path). Extends ADR-0002; supersedes neither (slice-0005's frozen account of the frame-as-boot-face stays true for its date). | ADR-0041 | verified_by: scripts/0005-cardputer-bringup-verify.mjs (`verify:device`: a normal build dumps the splash, a `SAPPER_TEST_PANEL` build dumps the proof); review-only (`setup()` probe dispatch + no diagnostic primitive on the shipped boot path) |
| 23 | The Maintenance dashboard's write controls mutate only as **reboot-scoped intents**, never live engine ownership (extends #13 / ADR-0021): *resume* (`POST /resume`) and *re-provision* (`POST /save`) each set a pending-reboot flag the `runMaintenance` loop honours by rebooting into Station *after* the HTTP response flushes — no control reaches into a running engine (there is none in this phase). Every mutating action is **POST**; only reads are GET (`GET /`, `GET /config`), so an OS captive-check probe or a browser prefetch (which fire GETs at arbitrary paths against a server whose `onNotFound` serves a page) cannot reboot or re-provision the device. Re-provision writes only through the `loadProvisioning()`/`persistProvisioning()` seam (#6) and renders the no-echo `SetupFormModel` (#20) — no new secret access site, no secret echoed. There is **no** distinct force-sync control: rebooting into Station already runs an always-due first sync (`SyncScheduler::begin` at each boot), so ADR-0039 decision 8's third control collapses into resume. Extends ADR-0021 #13 and ADR-0039 (its write half); supersedes neither. | ADR-0043 | verified_by: test/test_dashboard (controls render a POST resume form + `/config` link, not a GET link) + scripts/0040-maintenance-verify.mjs (`verify:maintenance`: `POST /resume` reboots, `GET /config` serves the form, `POST /save` persists + reboots); review-only (route registration POST/GET split + reboot-after-flush in `runMaintenance`) |
| 24 | A real panel init failure (`IDisplay::begin()` returns `false`) fails **loud then safe**: `setup()` logs `[FATAL] display init failed` once and continues fully headless, and every panel *surface* draws through the init-checked selection (`panelAfterInit()`), never the raw `display()` — so a failed panel is treated exactly as an **absent** panel (the proven screenless path) instead of half-drawing on the guarded-but-dead LovyanGFX buffer (§3 fail-loud). Shipped reference-taking surfaces (boot splash, `runMaintenance`'s Maintenance screen) receive the `NullDisplay` fallback and keep running; `huntLoopBegin` receives `nullptr` so the HUD is skipped and `[SCREEN] disabled` logs — reusing hunt_loop's existing "no display" contract rather than wiring a renderer onto a zero-geometry panel (no new rendering path). The hooks-only panel-rendering bench probes (`panel_probe` + the `screen`/`hunt_hud`/`capture` surface probes) are instead **skipped** on init failure (a `panelReady` guard around their dispatch): a probe cannot render-and-dump a dead panel, and driving `present()` (a hardware push) through an uninitialised driver is the very half-draw this row forbids. The serial `dump` read path is out of scope (it reads, does not draw; a no-op on a failed panel). Extends ADR-0002 (capability resolution) and ADR-0001 (observers-never-owners) to the runtime-failure case; supersedes neither. | ADR-0045 | verified_by: test/test_board_profile (`panelAfterInit` routes draws to the null fallback on failure, to the real panel on success) + scripts/0005-cardputer-bringup-verify.mjs (`verify:device`: a working panel still dumps the splash); review-only (`setup()`/`runStationBoot`/`runMaintenance` wiring + the panel-probe `panelReady` guard) |
| 25 | On-air verify artifacts carry no real third-party network identifier: a committed `artifacts/` file contains no real BSSID (a MAC is geolocatable via public Wi-Fi databases such as WiGLE, so publishing one pinpoints where the device ran and exposes a bystander network), no real neighbour SSID, and no device-identifying MAC-derived SoftAP name — redacted evidence uses the non-hex placeholder `xx:xx:xx:xx:xx:xx`, `<redacted-ssid>`, and the documented generic `Sapper-XXXX`, keeping the evidentiary *structure* (a capture happened, N discovered) without the identifier. The already-committed identifiers were scrubbed from all history before the first push (slice-0050). Extends ADR-0004's "commit artifacts as evidence" (which predated any on-air artifact); supersedes none. | ADR-0049 | verified_by: scripts/check-artifact-privacy.mjs — the pre-commit hook runs it against the *committed* tree (stele:ADR-0018), failing on any MAC-format BSSID (colon/hyphen/dotted) or hex SoftAP name in a **text** artifact, and hard-failing on any raw capture file (`.pcap`/`.pcapng`/`.cap`) under artifacts/ (§4 #9 — capture bytes never belong in committed evidence); review-only (SSID *text*, and any identifier rendered into a **PNG** — a screenshot is pixels no regex can read); the same guard now also runs in CI (`.github/workflows/ci.yml` `guards` job) against the pushed tree, and in the local `pre-push` hook against the working tree (ADR-0051, §4 #26), so it is no longer hook-only and index-based; pending (LEDGER) (redaction in the on-air verify scripts so no real identifier — text or pixel — ever reaches disk). See ADR-0049's 2026-09-21 amendments. |
| 26 | CI (`.github/workflows/ci.yml`, the `guards` job) runs the doc linter, the generated-index check, and the artifact-privacy guard (§4 #25) against the *pushed* tree on every push/PR, and the local `pre-push` hook runs the same three checks against the *working tree* before a push — together the scan the index-based pre-commit hook cannot be. A pathspec-limited `git commit -- <path>`, a `--no-verify`, or a clone that never installed the hook all reach the remote otherwise (ADR-0049 F4). CI is the authority (runs on the server, unskippable, scans the exact pushed ref, and guards PRs from clones without the hook); the `pre-push` hook is only the fast local mirror (skippable with `--no-verify`, and — scanning the working tree, not the pushed ref — it can miss a push of a ref other than the checked-out tree, e.g. `git push origin HEAD~1:main`, which is why CI is the backstop). Removing either CI gate reopens F4. | ADR-0051 | review-only (the CI `guards` job runs the gates on the pushed tree — the mechanism; that a later workflow edit does not remove them, the doc linter cannot check because it does not read `.github/`) |

When an ADR's consequences create a rule that all *future* work must follow, add the row
in the same commit as the ADR.
