# Security model and known limitations

This document describes the WiFi Sapper firmware's **security posture**: what it is designed to
protect, the handful of **deliberate exceptions** to that design (recorded here so they are not
mistaken for defects), and the **known limitations** a deployer should weigh before trusting a unit
in the field. It is a live document (stele:ADR-0010): it describes how the firmware behaves now.

It complements two neighbours and does not repeat them:

- [`SECURITY.md`](../SECURITY.md) is the vulnerability-reporting policy (what counts as a reportable
  firmware defect, and how to report it privately).
- [`DISCLAIMER.md`](../DISCLAIMER.md) is the legal/authorized-use boundary.

The Sapper is offensive tooling: handshake capture and operator-armed deauth are its intended
function, not weaknesses. The posture below is about protecting the **operator** and the **operator's
secrets**, not about hiding the attack features.

## What the firmware protects

These are design invariants, each enforced in the codebase (the parenthetical is the standing
invariant in [`../CLAUDE.md`](../CLAUDE.md) §4 and the ADR that set it). They hold by construction,
not by review vigilance:

- **Provisioned secrets never reach served HTML.** The captive setup form is rendered from a
  secret-free view-model, so the WiFi passphrase, wpa-sec key, and Maintenance passphrase cannot be
  echoed back to anyone on the setup access point. Only the *fact* that a secret is stored is shown
  (invariant #20, ADR-0037).
- **No plaintext password leaves the device over the network.** The off-device push webhook carries
  network identity only (ESSID + BSSID); its payload structurally has no password field (invariants
  #14/#19, ADR-0023/ADR-0035).
- **No plaintext password on the shoulder-surfable surfaces.** The on-panel HUD, the status LED, and
  the serial log never render a recovered PSK. Recovered passwords are shown only in the Maintenance
  dashboard (see the deliberate exception below) (invariant #21, ADR-0039).
- **The wpa-sec cloud link is certificate-pinned.** Uploads and the hourly cracked-results sync
  validate TLS against a single pinned anchor, so the operator's API key cannot be MITM'd by a rogue
  CA (ADR-0017; see the pinning limitation below).
- **Provisioning is never writable over serial.** The serial control channel is read-only
  observation (`ping`/`state`/`dump`); no serial command can write or clear credentials (invariants
  #1/#7, ADR-0003/ADR-0006).
- **Shipped builds carry no actuation vocabulary.** The serial *stimulus* tokens (inject a handshake,
  force a sync, inject a cracked result) exist only in `SAPPER_TEST_HOOKS` bench builds and are
  absent from every shipped binary, asserted on every native test run (invariant #4, ADR-0047). A
  shipped device cannot be commanded to upload, sync, or deauth over serial.

## Deliberate exceptions (recorded, not oversights)

Three places intentionally relax the "no plaintext / off by default" posture. Each is a recorded
decision with a threat model, not a gap to "fix":

- **The Maintenance dashboard serves recovered passwords in clear.** This is the point of the
  dashboard: a local results viewer on the device's *own* access point, reached only by a physical
  entry gesture at the device (ADR-0039 decision 5, invariant #21). It is the opposite threat model
  to the off-device webhook. Mitigation is in the operator's hands: set a strong **Maintenance
  dashboard passphrase** so only you can join the `Sapper-Maint-XXXX` access point. If you never set
  one, it falls back to the public default `sapper-setup`, and anyone in radio range who knows that
  default could read the recovered passwords. The on-device panel shows only a count, never the
  passwords, because the panel is over-the-shoulder visible.
- **The raw-transmit capability ships in every binary.** The low-level ability to transmit raw
  management frames (the SDK sanity-check bypass) is compiled into every device build, but a deauth
  frame is transmitted **only** when the operator has explicitly armed deauth through the portal
  (default **off**). A disarmed or unprovisioned device transmits nothing (invariant #16, ADR-0029).
  The capability ships because the appliance's authorized-pentest purpose needs deauth in the hunt
  loop; the default-off arm is what keeps a lost or misused unit from jamming.
- **The setup access point uses a public default password.** During first-boot provisioning the
  device raises `Sapper-XXXX` with the published passphrase `sapper-setup` so a freshly flashed
  device can be reached with no prior secret. Anyone in range during provisioning could join it;
  provision somewhere you control, and note that the no-echo guarantee above still means a joiner
  cannot read an already-stored secret. The setup page itself is served over **plain HTTP** (a
  captive portal on a private access-point IP cannot be HTTPS), so the WiFi passphrase and wpa-sec key
  you submit travel in clear over the setup link, protected only by that SoftAP's WPA2: a further
  reason to provision somewhere you control. This is why a CodeQL `cpp/non-https-url` finding on the
  portal URL is expected and dismissed, not a defect.

## Known limitations

These are tracked in [`../LEDGER.md`](../LEDGER.md) and surfaced here so a deployer can weigh them.
None is a secret; all are consequences of the current alpha scope.

### Secrets at rest are plaintext

Provisioned credentials (the WiFi passphrase and the wpa-sec API key, and the Maintenance passphrase)
are stored in **plaintext NVS** on the device flash. Anyone with physical possession and a flash dump
can read them. Flash encryption and secure boot are a tracked follow-up (ADR-0006). **Treat a
provisioned device as holding its network passphrase in the clear**, and wipe or re-flash a unit
before it leaves your custody.

### The wpa-sec CA pin expires, and fails closed

The wpa-sec TLS pin trusts one anchor, the self-signed **GTS Root R4**, which expires **2036-06-22**;
wpa-sec could also migrate off Google Trust Services sooner. If either happens, the pin stops matching
and uploads/sync **fail closed** (no key is exposed, but no capture is uploaded and no cracked result
is fetched). The signal is a TLS handshake failure after a period of success. The fix is a firmware
release that re-measures the live chain and updates the pinned PEM (`src/net/wpasec_root_ca.h`)
(ADR-0017).

### Self-hosted webhooks behind a private CA are unsupported

The push webhook validates TLS against the **system root-CA bundle**, so a self-hosted ntfy (or any
endpoint) behind a **private** certificate authority will fail its handshake until that root is in the
bundle (ADR-0023). Use a publicly-trusted certificate for the webhook endpoint (ntfy.sh and Discord
already are). Embedding a custom private root is a tracked follow-up.

### Verification green does not yet catch every ESP-IDF fault

An assurance limitation, not a runtime vulnerability. The on-device verify scripts gate on **our own**
`[FATAL]`/`[ERROR]` log prefixes, so a raw ESP-IDF error line (for example an `E (...) esp_littlefs:
mount failed` panic or assertion) can pass a gate as green. This is how a 2026-10-25 bench run
reported a *passing* sync over a filesystem that had silently corrupted and reformatted. The specific
mount case is now loud (ADR-0074 emits `[ERROR]` for it), but the general hardening, failing the gates
on the ESP-IDF error class while tolerating benign warnings, is tracked (ADR-0074, ADR-0004). Read a
green verify as "our checks passed", not yet as "the device logged nothing bad".

## On-air proof status

The capture to upload to sync engine is proven on air with committed evidence under
[`../artifacts/`](../artifacts/) on the **M5Stack Cardputer ADV** reference target. The three other
boards (M5StickC Plus2, LilyGo T-embed, ESP32-3248S035C "CYD") are compile-and-link verified in CI on
every push and have been brought up on hardware, but some of their on-air proofs (per-board
photographic HUD/panel confirmation, PSRAM boot, the CYD sync leg) are **pending** and tracked in
[`../LEDGER.md`](../LEDGER.md). See the board table in [`../README.md`](../README.md#supported-boards).
