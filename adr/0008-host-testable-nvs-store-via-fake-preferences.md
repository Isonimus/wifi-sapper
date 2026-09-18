---
id: '0008'
title: "Host-testable NVS store via a fake Preferences on the native lane"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

slice-0007 put the provisioned-secret store (`persistProvisioning`, `loadProvisioning`,
`clearProvisioning`) in `src/net/provisioning_store.cpp` over Arduino `Preferences` (NVS), and
its Design section deliberately left that wrapper **out** of the native unit lane:

> The `Preferences` wrapper itself is not unit-tested behind a fake: a speculative fake NVS
> abstraction would be exactly the over-abstraction the quality bar forbids before a second
> caller exists (rule of three); its behaviour is proved on hardware by Scenarios C and D.

That was the right call *at the time* — there was one caller and no observed defect, so a fake
would have been speculative generality. The rule of three exists precisely to stop that.

The slice-0007 freeze ran the `/wrap-up` adversarial pass (stele:ADR-0017) over the store code.
A correctness reviewer, blind to intent, found two real defects that the hardware scenarios (C, D)
do **not** exercise, because neither scenario induces a *failure* mid-write:

1. **Mixed-triad on a partial write.** `persistProvisioning()` combined its three `putString`
   results with `&&`, which short-circuits. On a re-provision, if the middle write (`wpasec_key`)
   failed, the SSID write was skipped, leaving the *previous* SSID and key beside the *new*
   passphrase — a triad that was never submitted yet still passes `validateCredentials()` and
   loads as usable. This falsifies the store's own documented guarantee ("a complete triad or the
   previous one — never a mix"). The "SSID written last" reasoning only holds on a first
   provisioning, where the old SSID is genuinely empty.
2. **Swallowed seed result.** The `SAPPER_TEST_HOOKS` boot seed discarded
   `persistProvisioning()`'s return, so a hooks build flashed with no credential flags onto an
   already-provisioned board kept the stale real credentials instead of routing to the portal —
   contradicting the documented behaviour and violating the quality bar's fail-loud rule.

Both were fixed in code. But a fix with no regression test is a fix that a later refactor
silently reverts — and these two behaviours (wipe-on-partial-write; clear-a-stale-triad) are
observable **only** under an induced NVS write failure or a pre-populated store, which the
hardware lane cannot inject on demand. That is the second caller the rule of three was waiting
for: the store now has logic that is worth guarding and cannot be guarded any other way.

## Decision

Bring the NVS store into the native unit lane (ADR-0004 lane 1) behind a **fake `Preferences`**,
so its failure and boundary behaviour is host-tested and runs in cloud CI.

- A native-only `native/mock/Preferences.h` provides an in-memory drop-in for the exact
  `Preferences` surface `provisioning_store.cpp` uses (`begin`/`getString`/`putString`/`clear`/
  `end`) plus one **fault-injection knob** (`failPutKey`) that makes a chosen key's write report a
  short count and store nothing — reproducing a mid-triad NVS failure. It is placed on the include
  path of `env:native` only (`-I native/mock`); device builds resolve `<Preferences.h>` to the
  real ESP-IDF header, untouched. The header `#error`s if compiled outside the `UNIT_TEST` lane so
  it can never leak into a shipped binary.
- `net/provisioning_store.cpp` is added to the native `build_src_filter`. It is not
  `#ifndef UNIT_TEST`-guarded (unlike the portal and STA glue, which pull in `WiFi`/`WebServer`),
  so with the fake header it compiles unchanged on the host.
- The `SAPPER_TEST_HOOKS` seed policy (persist the seed, or clear any stored triad if the seed is
  invalid) is extracted from `main.cpp` into a store function, `seedProvisioning()`, so the
  persist-or-clear composition — the site of defect 2 — is itself host-testable rather than buried
  in device boot glue.

This **revises the slice-0007 Design note quoted above**: the fake is no longer speculative, so it
is built. slice-0007 is a frozen record and its prose is not edited; this ADR is where the reversal
is recorded, with the reason (the rule of three is now met), per the project convention that a
changed decision is written as a new record rather than an edit.

## Consequences

- The store's partial-failure and stale-triad behaviour is now proved on every push (lane 1), not
  only by a hand-run hardware scenario — and specifically it is proved by tests that **fail before**
  each fix and **pass after**, which is the guarantee the hardware lane could not give.
- The fake mirrors only the calls the store makes and one fault knob; it is not a general NVS
  emulator. If a later store operation needs another `Preferences` call, the fake grows that one
  call with it — never ahead of a real caller (the same rule of three, now applied to the fake).
- The device build is unchanged: the fake is on the native include path only, and the real
  `Preferences` still backs the store on hardware, where Scenarios C and D continue to prove the
  end-to-end path against real NVS. The fake proves the *logic*; the hardware proves the *wiring*.
- Invariant #6 (single writer) is unaffected: `persistProvisioning()` remains the only `putString`
  caller, and `seedProvisioning()` composes the existing `persist`/`clear` seam rather than
  reaching into NVS itself.
