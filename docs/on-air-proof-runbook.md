# On-air proof runbook

This is the operator's bench procedure for the on-air proofs that cloud CI cannot run (§4
invariant #5: no CI lane depends on attached hardware). It is a live document
(stele:ADR-0010): it describes how these proofs are run *now*, against real hardware and
the live wpa-sec service.

**Written for the bench operator** running a provisioned board on a workstation, not for a
reader of the code. Each proof below states its build recipe, the exact run command, the
serial markers that mean pass or fail, and the artifact to keep.

## Scope

Three promises that only air can prove, each tracked as an open item in
[`../LEDGER.md`](../LEDGER.md):

1. **Scenario J** — the wpa-sec cracked-results download, de-chunk, parse, and manifest
   seed (slice-6, ADR-0019).
2. **The CYD sync leg** — the same download on the no-PSRAM classic-ESP32 board, which also
   exercises the ADR-0082 internal-heap manifest on a provisioned device.
3. **Gate C (`markSubmitted`)** — a terminal Accepted/Duplicate upload records the network
   as submitted, after which it is neither re-attacked nor re-uploaded (ADR-0065 #30).

What these three share, and why they are not unit tests: each depends on a live TLS session
to wpa-sec validated against the pinned GTS Root R4 (ADR-0017), a real association, and a
real NTP clock. The pure logic beneath them (parse, manifest policy, delta/alert, the gate
seams, the streaming response classifier) is already covered on the native lane; these
prove the on-air half.

## Before you start

- A workstation with PlatformIO and `npm install` done in this repo (the verify scripts
  need `serialport`).
- A board flashed from this repo, real WiFi in range, and a wpa-sec account with its API
  key. For the most meaningful Scenario J seed, the account should already hold at least one
  cracked result; an empty account still proves the download path (`downloaded=0`).
- Authorization to operate the radio where you run it (see [`../DISCLAIMER.md`](../DISCLAIMER.md)).

**Secrets stay in your own shell.** Set `SAPPER_TEST_WIFI_SSID`, `SAPPER_TEST_WIFI_PASS`,
and `SAPPER_TEST_WPASEC_KEY` as environment variables in the shell you build from. They are
build-time inputs to the `*_testhooks` env; they are never committed and never pasted into a
transcript or a chat.

**Serial.** All boards log at 115200 baud. The verify scripts resolve the port from the
first CLI argument, then `$SAPPER_PORT`, then auto-detection of an Espressif-looking device
(`manufacturer` matching Espressif, or a `ttyACM`/`usbmodem` path). The Cardputer's native
USB-CDC auto-detects; the CYD's CH340 bridge enumerates as `/dev/ttyUSB*` and is **not**
auto-detected, so you must pass its port explicitly (see Proof 2).

**If association fails (`[FATAL] ... could not associate or sync NTP`).** Read the raw serial for
the ESP-IDF reason code. `Reason: 201 - NO_AP_FOUND` means the radio scanned but found no AP matching
the configured SSID (the radio itself is fine, the provisioning SoftAP still comes up): check the SSID
is the **2.4GHz** network (the ESP32 cannot see 5GHz), spelled exactly, broadcast (not hidden), and on
channel 1-11 (a 2.4GHz AP that auto-picked channel 12/13 is invisible to the ESP32's default region).
A flaky AP can also drop its 2.4GHz radio into this state, observed on this bench, cured by rebooting
the AP. An `AUTH_FAIL` / `4WAY_HANDSHAKE_TIMEOUT` reason instead points at the passphrase. Note the
`*_testhooks` build re-seeds provisioning from the build-time `SAPPER_TEST_WIFI_*` flags every boot
([main.cpp](../src/main.cpp) `seedTestCredentials`), so a portal-provisioned board does **not** carry
its stored creds into a testhooks build: pass them at build time, single-quoted so the shell leaves
special characters intact.

## Proof 1 — Scenario J: cracked-results download and manifest seed

Board: **cardputer_testhooks** (the reference target). The sync probe (`sync_probe.cpp`,
active when `SAPPER_TEST_SYNC` is set) advances the scheduler clock so a sync is due at once
rather than after a real hour, opens one STA window, downloads `?api&dl=1` over the pinned
TLS link, de-chunks and parses the body, seeds the LittleFS manifest, and prints a summary.

**Build and flash** (secrets from your shell environment):

```
SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
SAPPER_TEST_SYNC=1 pio run -e cardputer_testhooks -t upload
```

**Run** (the script observes the already-running board; it does not reset it):

```
npm run verify:sync          # or: node scripts/0020-sync-verify.mjs /dev/ttyACM0
```

**Pass:** the script prints `PASS` and writes [`../artifacts/0020-sync.txt`](../artifacts/).
The load-bearing serial line is:

```
[SYNC] verify sync ok first=1 downloaded=N new=M malformed=0 overflow=0
```

`downloaded=0` is still a pass: it proves download, parse, and persist over an empty
account. `malformed>0` signals a wpa-sec body-format change worth investigating. Seed one
crack if you want a non-empty manifest exercised.

**Fail:** any line beginning `[FATAL]` or `[ERROR]` (a TLS pin mismatch, a failed or garbage
download, a parse fault), or no `[SYNC]` output inside the 90 s window (check that the build
carried `SAPPER_TEST_SYNC=1` and real credentials).

**Artifact privacy (§4 #25).** `0020-sync.txt` captures `[SYNC]`/`[CRACK]`/`[HUNT]` lines; a
`[CRACK]` line can carry a real SSID. Before committing it, replace any real identifier with
the documented placeholders (`xx:xx:xx:xx:xx:xx`, `<redacted-ssid>`) and run
`npm run check:artifact-privacy`. The pre-commit hook runs the same guard and hard-fails on
a MAC-format BSSID or any raw capture file.

## Proof 2 — The CYD sync leg

Board: **cyd_3248s035c_testhooks** (ESP32-3248S035C, classic ESP32, no PSRAM, CH340 UART
bridge). Same probe and markers as Proof 1; the point is to prove the download leg on the
board whose memory profile forced ADR-0082 (the cracked manifest placed in the internal DRAM
heap), so a clean sync here also confirms that manifest reads back correctly on a provisioned
device.

**Build and flash:**

```
SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
SAPPER_TEST_SYNC=1 pio run -e cyd_3248s035c_testhooks -t upload
```

**Run — the port must be explicit** (the CH340 is not auto-detected):

```
SAPPER_PORT=/dev/ttyUSB0 npm run verify:sync
```

**Pass:** `[SYNC] verify sync ok ...` as in Proof 1. The shipped sync path also logs its own
outcome line, which on success reads:

```
[SYNC] outcome ok=1 first=1 downloaded=N new=M malformed=0 overflow=0 storeError=0 fetch=0
```

`fetch` is the download transport verdict, a `FetchResult` enum printed as its integer:
**0 = Ok** (the success value above), **1 = Transport** (a network/TLS transfer failure),
**2 = Auth** (a wpa-sec key rejection). On a clean sync `ok=1` and `fetch=0`; `first=1` only on
the very first sync that seeds a fresh manifest (a later re-sync reads `first=0`, `new=0`).

**Known transient (not a defect).** A first run may show `[SYNC] outcome ok=0 ... fetch=1`
(fetch = Transport) alongside an mbedTLS `(-80)` line (`MBEDTLS_ERR_NET_CONN_RESET`, a TCP reset
refused by the wpa-sec server, LEDGER / ADR-0017). This is board-independent and self-healing:
the sync stays due and retries next window, so re-run and watch for `ok=1 ... fetch=0`. Distinguish
it from a pin failure, which presents as an X509 verify error (`-0x2700`) and is **persistent**: a
pin failure is a real fail, the `-80` reset is not.

**Artifact privacy:** same scrub as Proof 1.

## Proof 3 — Gate C: `markSubmitted` and re-submit suppression

The promise (ADR-0065 #30): a handshake that wpa-sec answers with a terminal **Accepted** or
**Duplicate** marks that BSSID as submitted; from then on the engine skips it at target
selection and the drain drops any queued capture for it, so the appliance never re-attacks or
re-uploads a network it has already landed. This generalises gate A (already-cracked,
ADR-0059/ADR-0061): the same drain and target-selection sites, a different lookup set.

**Why this needs a real uplink.** `markSubmitted` writes the submitted set only on a terminal
Accepted/Duplicate, never on Rejected (gate on knowledge, not hope). A *synthetic* injected
handshake is uncrackable and is classified Rejected, so it can never fire gate C. The trigger
is therefore a genuine Accepted/Duplicate from the live service: run this where an authorized
network has an active client, or aim at a network wpa-sec already holds so the upload comes
back **Duplicate** (the easiest deterministic trigger).

Board: **cardputer_testhooks**. Two builds surface the drain-outcome counters on serial, and
either proves gate C:

- A **normally-booting** hooks build (no probe env): the shipped hunt loop's logger prints the
  `[UPLOAD] drain done` line under `SAPPER_TEST_HOOKS` (ADR-0103, gated per ADR-0101's single
  compile flag). This is the shipped-shaped path `verify:serial-stimulus` drives, what a field unit
  actually does, with the counters compiled in for the bench, so it is the preferred build.
- The **upload probe** (`upload_probe.cpp`, active when `SAPPER_TEST_UPLOAD` is set): also runs
  the real hunt and drain loop and prints the same line every cycle, with a snappier re-drain
  cadence (its recipe is below).

Before ADR-0103 only the probe surfaced these counters (the shipped-loop logger kept drain facts
off serial, ADR-0021); either build works now.

**Build and flash:**

```
SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
SAPPER_TEST_UPLOAD=1 pio run -e cardputer_testhooks -t upload
```

**Observe** (there is no dedicated verify script for gate C; watch the serial log, or tee it
to a file you later scrub). The device self-injects a synthetic stimulus for liveness
(always Rejected, so it never pollutes the submitted set) and reports every drain:

```
[UPLOAD] drain done associated=1 accepted=A duplicate=D rejected=R purged=P \
         skippedCracked=C skippedSubmitted=S storeErrors=0 submittedStoreErrors=0 resumeFailed=0
```

**Pass — two signals, in order:**

1. **`markSubmitted` fires.** A real capture uploads and wpa-sec answers terminally:
   `[UPLOAD] wpa-sec result=accepted` (or `=duplicate`) for a given BSSID.
2. **Suppression engages.** Across the following cycles that BSSID is **not** re-uploaded
   (no further `wpa-sec result=` line for it) and is no longer targeted. If a capture for it
   still reaches the queue before target selection drops it, the drain line shows
   `skippedSubmitted` incrementing; `skippedCracked` increments the same way once a network
   is in the downloaded cracked manifest (gate A).

Note the two gates are belt-and-suspenders: target selection usually prevents the re-capture,
so in the common case `skippedSubmitted` stays `0` and the proof is **behavioural** (the
network is landed once and never again). The drain counter is the direct signal and fires
only when a capture slips through the target-selection skip in the same window.

**Fail:** any `[FATAL]`/`[ERROR]`; or a clean upload that comes back `rejected` for a network
that should crack/duplicate (a classification fault worth investigating, e.g. the ADR-0076
response-buffer regression this board once had); or a BSSID that keeps re-uploading after a
terminal Accepted/Duplicate (suppression not engaging).

**Artifact privacy:** if you tee the log under `artifacts/`, scrub every real BSSID to
`xx:xx:xx:xx:xx:xx` and SSID to `<redacted-ssid>` before committing, and run
`npm run check:artifact-privacy`. Raw captures are never committed.

### The CYD variant

The same gate-C question is open specifically on the CYD (LEDGER: confirm suppression engages
on the CYD with a reachable uplink, after a 2026-10-27 session where a weak uplink meant no
upload landed, so the gates correctly never suppressed). Repeat Proof 3 on
**cyd_3248s035c_testhooks** (explicit `$SAPPER_PORT=/dev/ttyUSB0`) in a strong-uplink spot to
close it, confirming no CYD-specific TLS/classification quirk returns Rejected on a clean
upload.

## Related: `verify:serial-stimulus`

`scripts/0048-serial-stimulus-verify.mjs` (`verify:serial-stimulus`, the §4 #4 on-air proof that
the `SAPPER_TEST_HOOKS` stimulus vocabulary drives the *shipped* loop) runs against a
normally-booting hooks build: it sends `inject-handshake` then `force-sync` and waits for the
shipped-loop `[UPLOAD] drain done` line that ADR-0103 added under the hooks flag. It was formerly
blocked because that line existed only in the upload probe (ADR-0021 kept drain facts off the serial
logger); ADR-0103 resolves it, and the same line is what gives Proof 3 a direct gate-C signal on the
shipped-shaped build. It is a hardware-lane script (§4 #5), run on an attached board, never in cloud
CI.
