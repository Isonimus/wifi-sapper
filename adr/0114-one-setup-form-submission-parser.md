---
id: '0114'
title: "One pure setup-form submission parser, shared by both portals"
type: architecture
status: accepted
date: 2026-11-07
supersedes: []
superseded_by: []
---

## Context

The setup form is served and saved by two surfaces: the captive portal (first boot and the
STA-fail fallback, ADR-0006) and the Maintenance `/config` editor (ADR-0043). Both render from
the pure, shared `buildSetupForm`/`resolveProvisioningUpdate` seam (ADR-0037, §4 #20), but each
`handleSave` **hand-parsed the POST into a `ProvisioningRecord` independently** — the same list of
`WebServer::arg(name)` reads, the same bounded copies, the same checkbox and dim-delay parsing,
written out twice. ADR-0043 decision 6 chose to keep that duplication: it is the *second* instance
of the arg plumbing, and the quality bar's rule of three (`docs/quality-bar.md` §3) says to
duplicate at two and extract at three, on the premise that the wrong abstraction costs more than a
duplication. A `LEDGER.md` item tracked it, to be hoisted into a shared helper "only when a third
serving surface appears".

That premise held only while the two copies were independent. They were not: they share an
**invisible contract — the full field list** — and must stay in lockstep as the record grows. They
did not. The measured evidence:

- **The screen-dim delays (ADR-0093, slice-0094) were added to the captive portal's `handleSave`
  but never to the Maintenance one.** So a Maintenance `/config` re-save submitted the dim fields,
  the parser dropped them, the zeroed `submitted` flowed through `resolveProvisioningUpdate` (the
  dim delays are not keep-on-blank — they come straight from the submission), and dimming was
  **silently reset to off** on every Maintenance re-save. A real, shipped, silent data-loss bug,
  latent since slice-0094 and found by the slice-0113 boyscout read.
- **The Maintenance `handleConfig` also never prefilled the dim fields into its `SetupFormModel`**,
  so the form rendered them blank even before a save — the same omission on the render side.
- Each later field (`pmkid`, ADR-0110; `pwncrack`, ADR-0109) had to be hand-added to *both*
  handlers, every addition a fresh chance to drift the same way.

So the duplication's cost became **correctness**, not lines: a shared, growing field list hand-copied
across two sites produces a latent silent bug on every addition. That is precisely the condition
under which the rule of three's own premise inverts — the "wrong abstraction" the rule guards
against is not present here, because the two sites are not divergent variants that a premature
abstraction would wrongly fuse; they are meant to be identical and only drifted by omission. When
duplicated instances must be identical over a shared contract, divergence is the default failure, so
the abstraction that makes them identical by construction earns its place at two, not three.

## Decision

**Extract one pure `parseSetupFormSubmission`, owned by the `net/provisioning_form` module, and
route both portals through it over a `FormArgReader` seam. This revises ADR-0043 decision 6.**

1. **`parseSetupFormSubmission(const FormArgReader& args, ProvisioningRecord& submitted, bool&
   openNetwork)`** is the single owner of the submitted-field name list and the per-field parsing:
   the bounded string copies (over-length rejected, never truncated — a truncated SSID could slip
   past `validateCredentials` as a shorter network), every checkbox (`"on"` = set, absent = clear),
   and both clamped screen-dim delays (`parseDelaySeconds`). It is pure — no `WebServer`, no NVS —
   and host-tested. It does **not** merge, validate, or persist: the caller still runs
   `resolveProvisioningUpdate` over the stored record, `validateCredentials`/`validateNetworkEntry`,
   and `persistProvisioning`. So the field *list* joins the merge *policy* in the shared pure module;
   only the HTTP transport stays per-site.

2. **`FormArgReader`** is the abstract read-only view that keeps the parser pure: `value(name)`
   returns the submitted value, or `""` (never `nullptr`) for an absent field, mirroring
   `WebServer::arg`. The device supplies `WebServerArgReader` (`net/web_arg_reader.h`, device-only),
   which copies each arg into a reused scratch `String`; the native lane supplies a fake map. The
   returned pointer need only stay valid until the next `value()` call — the parser copies or
   consumes each value before reading the next, so no two values are held at once.

3. **Both `handleSave` handlers become:** construct a `WebServerArgReader`, call
   `parseSetupFormSubmission`, and on `FieldTooLong` serve the existing 400 (the only per-site
   difference is the `Back` href, which stays at the call site). The hand-listed arg block and the
   per-portal `copyBounded` helper are deleted from both `.cpp` files.

4. **This revises ADR-0043 decision 6, and supersedes nothing.** ADR-0043's other decisions — the
   POST/GET split, reboot-scoped intents, and §4 #23 — still stand, so ADR-0043 stays `accepted`; an
   `## Amendment` on it points forward to this ADR. Only its "keep the save-wiring duplicated until a
   third surface" sub-decision is reversed, on the evidence above.

## Consequences

- **Adds §4 standing invariant #48** in this commit: a setup-form submission is parsed by the one
  pure `parseSetupFormSubmission` seam over a `FormArgReader`; no portal hand-parses `WebServer`
  args into a `ProvisioningRecord`, so the field list cannot drift between serving surfaces. A future
  third serving surface uses the seam; a reader must not re-inline the parse.
- **Fixes the Maintenance `/config` re-save-wipes-dimming defect** (both halves: `handleSave` now
  parses the delays through the shared seam, and `handleConfig` now prefills them into the model),
  and closes its `LEDGER.md` `[bug]` line and the ADR-0043-decision-6 duplication `[deferred]` line.
- **Tightens §4 #20's enforcement.** The portal save-wiring's *parse* half is now host-tested
  (`test/test_provisioning_form`) rather than review-only; what stays device/review-only is the
  `WebServerArgReader` adapter, the `SetupFormModel` building in `handleRoot`/`handleConfig`, and the
  merge/validate/persist call sequence — proven on air by the slice-0007/0040 provisioning verifies.
- **The rule of three is unchanged as a default; this records the exception.** Duplicate at two
  still holds for independent variants; the carve-out is narrow — instances that must stay identical
  over a shared, growing contract, where the first silent drift is the signal to extract.
