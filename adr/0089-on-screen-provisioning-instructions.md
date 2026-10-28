---
id: '0089'
title: "On-screen provisioning instructions during the first-boot portal"
type: architecture
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Context

An unprovisioned device enters the Provisioning phase and `startPortal()` brings up the SoftAP,
but it draws nothing to the panel: the last thing rendered is the boot splash (`drawSplashFrame`,
ADR-0041), which simply persists. The join details (`ssid=Sapper-XXXX pass=sapper-setup
url=http://192.168.4.1/`) are logged to **serial only** (`startPortal()` `[PORTAL]` banner).

That serial banner serves a headless operator, who reads the log over USB. It does nothing for an
operator holding a device **with a screen**: they power it on, see "WiFi Sapper v0.1.0", and have no
way to learn the SoftAP name, its password, or the setup URL. The documented provisioning guide lives
in the README (ADR-0006 #5), but the dominant distribution path for this firmware is a one-click flash
from m5burner, which never surfaces the README. So the most common screen-equipped first-boot today is
a dead end: the information needed to provision exists only on a channel that operator is not watching.

Two facts make the fix safe and small:

- The provisioning SoftAP password is the **fixed, public default** `kSoftApPassword` (`"sapper-setup"`),
  a documented constant, not a per-device secret. Rendering it on the shoulder-surfable panel leaks
  nothing: anyone who can read the README (or decompile the firmware) already has it, and the AP is
  unattended for seconds during setup only. This is deliberately the **opposite** of §4 #21, where the
  Maintenance dashboard's PSKs are **per-device secrets** kept off the panel precisely because the panel
  is shoulder-surfable. The distinction is secret-vs-public, not panel-vs-not-panel.
- The render is static during provisioning (the SSID/password/URL do not change), and the sibling
  boot-face screens (`drawSplashFrame`, `drawMaintenanceScreen`) already establish the pattern: a single
  `fillScreen` + a few `drawText` lines, drawn once, with a `NullDisplay` making it a no-op on an absent
  or failed panel (ADR-0045).

§4 #22 (ADR-0041) pins the shipped boot face to render **only** the product splash. Adding a second
boot-face state is therefore not a silent layout tweak (the reason slice-0088's HUD restyle needed no
ADR): it changes a standing invariant, so it is recorded here and #22 is refined.

## Decision

1. **The Provisioning phase gets its own on-screen face.** A new `drawProvisioningScreen(IDisplay&,
   const char* ssid, const char* password, const char* url)` renders guided, newbie-first instructions:
   a `SAPPER SETUP` heading, then two numbered steps, "1. Join WiFi: `<ssid>` / pass: `<password>`" and
   "2. Open in browser: `<url>`". It shows the public default password, which is safe (see Context) and
   is the whole point: the screen operator who never read the README learns everything the serial banner
   tells the headless one.

2. **`startPortal()` draws it once, after the SoftAP is up.** `startPortal(IDisplay& panel)` takes the
   init-checked panel (the `NullDisplay` fallback on a failed panel, ADR-0045) and, after `g_portal.begin()`
   succeeds, draws the screen from the same sources the serial banner uses: `g_portal.apSsid()`,
   `kSoftApPassword`, and the AP IP. No new data source and no magic strings (DRY with the banner). The
   serial `[PORTAL]` banner is unchanged, so the headless path is untouched.

3. **Fixed-coordinate layout, matching the sibling boot-face screens.** The screen uses fixed
   coordinates tuned for 240x135, drawn top-left, exactly like `drawSplashFrame` and
   `drawMaintenanceScreen` (which are **also** not resolution-aware). This keeps all three boot-face
   screens in one consistent style and is the simplest thing that works (KISS). Making only this screen
   resolution-aware would leave it the lone resolution-aware boot screen beside two fixed siblings; the
   coherent move is one slice that ports slice-0088's two-tier pattern to **all three** boot-face screens
   at once, which is deferred (LEDGER).

This **extends** ADR-0041 and ADR-0006; it **supersedes neither**. ADR-0041's actual decision is that the
boot face is a *product face, not a diagnostic* (the splash replaced the RGB/border/diagonal test pattern,
which moved behind `SAPPER_TEST_PANEL`). A setup-instructions screen is a product face, not a diagnostic,
so that decision holds verbatim; only the consequence invariant #22's word "only" is refined to admit the
Provisioning-phase screen. ADR-0006's README provisioning guide stands; this is its on-panel mirror for the
operator who never opens it. The mechanism follows ADR-0041's own precedent for a boot-face screen:
device-only, `drawText`-composed, photographically verified, not host-tested (the render has no view-model
logic).

## Consequences

- §4 #22 is **refined**: the shipped boot face renders the product splash and, in the Provisioning phase,
  the setup-instructions screen (both product faces, never a diagnostic). The row is updated in the same
  commit to cite ADR-0089. The `SAPPER_TEST_PANEL`-only panel proof and the no-test-pattern-on-a-shipped-
  build guarantee are untouched.
- A recorded, scoped exception to the shoulder-surf doctrine: the **public default** provisioning password
  is shown on the panel. This does **not** weaken §4 #21, which concerns **per-device** Maintenance PSKs
  and keeps them off the panel; a future reader must not "fix" this by hiding the default password, nor cite
  it to justify showing a real secret.
- `README.md`'s provisioning section is updated in the same change (live doc): a screen-equipped board now
  shows the join instructions on the panel, so the README is the fallback, not the only source.
- Verification mirrors the splash/Maintenance screens: the render is photographic (eyes-on), as its
  correctness is pixels, not a host assertion; the portal's machine-checkable boot/log half stays covered
  by `scripts/0007-provisioning-verify.mjs` (`[STATE]`/`[PORTAL]`, no `[FATAL]`), unchanged and already
  wired.
- A deferral is recorded in `LEDGER.md`: port slice-0088's resolution-aware two-tier layout to all three
  fixed-coordinate boot-face screens (splash, Maintenance, provisioning) in one slice, so a larger panel
  uses its full area at boot, not only during the hunt.
