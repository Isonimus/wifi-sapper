---
id: '0120'
title: "Fix the captive-portal stack overflow that crashed first-boot provisioning on connect"
type: slice
status: accepted
date: 2026-11-12
supersedes: []
superseded_by: []
---

## Goal

A freshly flashed, unprovisioned unit **rebooted the instant a phone joined its `Sapper-XXXX`
setup AP**, so first-boot provisioning was impossible on the CYD and the T-embed (every board
reproduces it; those two were the two unprovisioned units on hand). Fix the crash at its root.

This closes the LEDGER `[bug] Provisioning SoftAP crashes on station-connect` item, and
**corrects that item's diagnosis**: the backtrace landing in the Wi-Fi driver's `esf_buf_alloc`
led the ledger to a low-internal-DRAM hypothesis (esp-idf#2545/#3810), but at provisioning the
unit has `heap_free=230856` / largest block `110580` — not memory-starved. The real cause is a
**loopTask stack overflow**: `CaptivePortal::handleRoot()` placed a 5632-byte setup-form buffer
(`char html[kSetupFormBufSize]`) on the ~8 KB Arduino loopTask stack, in the same frame as a
`ProvisioningRecord` and a `SetupFormModel`, underneath the `WebServer::handleClient()` call
chain. A hard stack overflow writes past the stack into adjacent memory, so the fault surfaces
later and misleadingly inside the Wi-Fi driver (`esf_buf_alloc`), and the `_xt_alloca_exc`
double exception is the panic handler itself overflowing. It fires "on connect" because the OS
captive-portal probe issues an HTTP GET the moment the phone associates, which runs `handleRoot`.

Independent of slice-0119 (confirmed: the crash reproduces identically on the v0.3.0 factory
image, commit 90bcd6e, which predates both the HUD redesign and the collision fix), so it was
pre-existing, not a regression.

## Definition of Done

- **Given** a freshly flashed, unprovisioned CYD or T-embed in the Provisioning phase,
- **When** a phone joins the `Sapper-XXXX` SoftAP and the OS captive-portal probe (or a browser)
  requests the setup page,
- **Then** the unit serves the setup form and **stays up** (no reboot, no Core-1 Double
  exception), instead of crashing on connect.

## Design

The sibling handler had already solved this and documented it: `MaintenancePortal::handleRoot()`
allocates the identical `kSetupFormBufSize` buffer on the **heap**, with a comment reading "a
5 KB buffer on the stack under the WebServer frames overflows it (same cause as handleRoot)". So
the fix is to make `CaptivePortal::handleRoot()` match its already-proven sibling: `malloc` the
form buffer, fail loud on a null allocation (§3), `free` it on every exit path (the error branch
and the success branch), and keep the single-threaded malloc/free-per-request shape the
Maintenance portal uses (the portal serves one request at a time, so there is no reentrancy).

Considered and deliberately **not** taken here: extracting a shared "render the setup form and
send it" helper so both portals allocate through one place and the stack-vs-heap choice can never
drift again (the ADR-0114 abstract-at-two argument, since this is exactly a drift between two
instances of a shared contract: the Maintenance copy was fixed, the captive copy was not). That
hardening is deferred to the LEDGER to keep this fix surgical: touching only the broken handler
makes it a clean on-hardware test of the diagnosis, leaving the working Maintenance portal
untouched. `kSetupFormBufSize` and the WebServer/loopTask stack sizing are unchanged.

## Verification

This is device-only memory-placement glue: the overflow is a physical loopTask-stack phenomenon
that the native lane cannot reproduce (it does not compile `captive_portal.cpp`, which is under
`#ifndef UNIT_TEST`, and the choice of heap vs stack has no host-observable behaviour), so there
is no host regression test that fails-before/passes-after. Its proof is on-hardware, like the
other device-radio/stack rows:

- **On-hardware eyes-on (operator):** a freshly provisioned-mode CYD and T-embed each serve the
  setup form and stay up when a phone connects to the setup AP. Confirmed 2026-11-12 on both the
  CYD (direct-panel, classic ESP32) and the T-embed (S3): both now stay up and complete
  provisioning, where both rebooted on connect before.
- **`scripts/0007-provisioning-verify.mjs`** (already wired, `verify:provisioning`): the
  boot/`[PORTAL]`-up error gate (`[SCREEN]`/`[PORTAL]` logs, no `[FATAL]`) stays green on a hooks
  build. It does not drive a station-connect, so it does not by itself exercise the crash; the
  connect-path proof is the eyes-on above.

## As built

Shipped as designed. `CaptivePortal::handleRoot()` (`src/net/captive_portal.cpp`) now allocates
the setup-form buffer with `malloc(kSetupFormBufSize)`, returns a loud 500 on a null allocation,
and `free`s it on both the render-failure and the success paths, mirroring
`MaintenancePortal::handleRoot()`; `<cstdlib>` was added for `malloc`/`free`. A comment at the
site records the root cause (loopTask stack overflow surfacing in `esf_buf_alloc`) so a future
reader does not "restore" the stack buffer, and the Maintenance portal is untouched.

Definition of Done met: confirmed on hardware 2026-11-12 that a connecting phone no longer
reboots the CYD or the T-embed and the setup form serves. Verification: `cyd_3248s035c` and
`t_embed` both build SUCCESS; `npm run lint` 0/0; the native suite is unaffected by construction
(the change is under `#ifndef UNIT_TEST`). The anti-drift hardening (one shared form-render
helper across both portals) is deferred to the LEDGER.
