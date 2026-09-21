# Contributing to WiFi Sapper

Thanks for your interest. A few things make a contribution land smoothly.

## Before anything: authorized use only

WiFi Sapper is pentesting tooling — it captures WPA/WPA2 handshakes and, when armed, transmits
deauthentication frames. By contributing you accept that it is for **authorized testing, education,
and research** only — the terms in the [DISCLAIMER](DISCLAIMER.md). Contributions whose only purpose
is to harm third parties, evade the law, or target people without consent will be declined.

## Development setup

Install [PlatformIO](https://platformio.org/) (the VS Code extension, or `pip install platformio`)
and [Node.js](https://nodejs.org/) (for the doc linter and the verify-script registry). Clone the
repo, run `npm install`, and use the environments defined in `platformio.ini`:

```bash
pio test -e native      # host unit suite (Unity) — the pure logic (ADR-0004 lane 1)
pio run  -e cardputer   # reference firmware — M5Stack Cardputer ADV (ESP32-S3)
npm run lint            # documentation linter (ADR/slice corpus + citations)
```

The Cardputer ADV is the only board profile today; broadening to others is tracked in
[`LEDGER.md`](LEDGER.md) (slice-8). Hardware-in-the-loop behaviour is proven by the `verify:*`
scripts in [`package.json`](package.json) — see [README.md](README.md) for what each one needs. There
is no cloud CI yet (it is a tracked follow-up in the ledger), so **run the checks locally before you
open a PR.**

## Code standards

The full engineering bar lives in [`docs/quality-bar.md`](docs/quality-bar.md) and the repo-specific
conventions in [`CLAUDE.md`](CLAUDE.md). The essentials: C++17 over a HAL-first architecture with a
pure, host-tested core behind hardware seams; event-driven surfaces (never called directly by the
engine); fail-loud error handling; no magic values; and a regression test for every piece of logic —
one that fails before the change and passes after.

- Match the surrounding style and naming.
- Keep protocol/policy logic in the pure core (unit-tested on the `native` lane); keep the hardware
  seams to raw bytes only.
- Add or extend the Unity suites under `test/` for any logic you change; behaviour a unit test cannot
  assert ships a wired `scripts/<slice>-verify.mjs` (see [`CLAUDE.md`](CLAUDE.md) §3).

## Commits and pull requests

- **Conventional commits**: `feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `chore:`, with an
  optional scope (`fix(serial): …`). One logical change per commit.
- Explain **why** in the body, not just what.
- Fork → branch (`feature/…` or `fix/…`) → open a PR against `main`.
- Fill in the PR template, and make sure `npm run lint`, `pio test -e native`, and
  `pio run -e cardputer` pass locally first.

## A note on `adr/` and `slices/`

Those directories are the project's **decision records** — the [stele](docs/) ADR/slice workflow the
maintainer uses: a decision (ADR) or feature unit (slice) is written down *before* the code and
shipped in the same commit, and the immutable ones are enforced by a pre-commit linter. You are
welcome to read them for context, and they explain *why* the code is shaped the way it is, but you do
**not** need to author them to contribute — describe your change in the PR, link the issue it
addresses, and the maintainer will record any durable decision it settles.

## Reporting bugs and requesting features

Use the issue templates. For anything security-sensitive in the firmware itself, follow
[SECURITY.md](SECURITY.md) instead of opening a public issue.
