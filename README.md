# WiFi Sapper

<p align="center">
  <strong>Autonomous, headless-first WPA/WPA2 handshake-hunting ESP-32 appliance</strong>
</p>

<p align="center">
  <a href="https://github.com/Isonimus/wifi-sapper/actions/workflows/ci.yml"><img src="https://github.com/Isonimus/wifi-sapper/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/Isonimus/wifi-sapper/actions/workflows/codeql.yml"><img src="https://github.com/Isonimus/wifi-sapper/actions/workflows/codeql.yml/badge.svg" alt="CodeQL"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/status-alpha-orange" alt="Status: alpha">
  <img src="https://img.shields.io/badge/platform-ESP32--S3-333?logo=espressif" alt="Platform: ESP32-S3">
  <img src="https://img.shields.io/badge/built%20with-PlatformIO-orange?logo=platformio" alt="Built with PlatformIO">
  <a href="https://www.npmjs.com/package/@isonimus/stele"><img src="https://img.shields.io/badge/method-Stele-6E44FF" alt="Built with Stele"></a>
</p>

<p align="center">
  <a href="#provisioning-first-boot">Provisioning</a> •
  <a href="#normal-operation">Operation</a> •
  <a href="#flashing-a-board">Flashing</a> •
  <a href="#building--verifying">Building</a> •
  <a href="#built-with-stele">Method</a> •
  <a href="DISCLAIMER.md">Legal</a>
</p>

A portable, headless-first ESP-32 firmware appliance: it hunts WPA/WPA2 handshakes endlessly,
auto-uploads each capture to [wpa-sec.stanev.org](https://wpa-sec.stanev.org), and once an hour
fetches cracked results back and announces any newly recovered passwords. The capture→upload→sync
engine has no UI dependency, so the same firmware runs on hardware with or without a screen — and
if a panel *fails to initialise* at boot, the unit logs `[FATAL] display init failed` and keeps
hunting fully headless rather than stalling on the dead screen (ADR-0045).

The project conventions, decision records, and roadmap live in
[`CLAUDE.md`](CLAUDE.md), [`adr/`](adr/), and [`LEDGER.md`](LEDGER.md). This README is a live
document (stele:ADR-0010): the sections below describe how the firmware behaves **now**.

> [!WARNING]
> WiFi Sapper is pentesting tooling in **alpha**. Capturing handshakes from, or transmitting deauth
> at, networks you do not own or lack **explicit written permission** to test is illegal in most
> jurisdictions. Deauth is off by default; leave it disarmed unless every network in radio range is
> authorized. Read the [DISCLAIMER](DISCLAIMER.md) before use — by building or running this firmware
> you accept it.

## Provisioning (first boot)

The Sapper stores no credentials in its firmware image. On a device with no saved network — a
fresh flash, or one whose stored network has become unreachable — it opens a **captive setup
portal** instead of trying to hunt (ADR-0006):

1. The device raises a WiFi access point named **`Sapper-XXXX`**, where `XXXX` is the last four
   hex digits of its MAC address (so several units on a bench stay distinct).
2. Join it with the passphrase **`sapper-setup`**.
3. A setup page opens automatically (any address works — DNS is hijacked to the portal). If it
   does not, browse to **`http://192.168.4.1/`**.
4. Enter the network the appliance should join (SSID + passphrase; leave the passphrase blank for
   an open network) and your **wpa-sec API key** (from your wpa-sec.stanev.org account — the
   appliance cannot upload captures without it).
5. Optionally, a **push webhook URL** (ADR-0023): an [ntfy](https://ntfy.sh) topic URL
   (`https://ntfy.sh/your-topic`, or a self-hosted instance) or a Discord webhook URL. When set, the
   appliance pushes a notification naming the network (ESSID + BSSID). The URL must be `https://`; leave
   it blank to disable push. The plaintext password is **never** sent — only which network is involved.
   Choose *what* gets pushed with the **Push these** checkboxes (ADR-0035): **Cracked passwords** (on by
   default — a recovered password came back), **Handshake captures** (off by default — a fresh handshake
   landed, one push per network per run), and **Sync errors** (off by default — the hourly
   cracked-results sync ran but failed, e.g. wpa-sec unreachable or erroring, while the device is
   otherwise associated; a *total* loss of connectivity raises no push, since with no network there is no
   window to send one — that is what the on-device LED/screen are for). These gate the off-device push
   only; the on-device screen, LED, and serial log always show every event.
6. Optionally, **Enable deauth (arm)** (ADR-0029): when checked, the hunt loop broadcasts deauth
   frames at every AP it discovers, knocking clients off so they re-associate and their handshake can
   be captured — the appliance's active industrial-pentest mode. It is **off by default**: a fresh or
   unchecked device hunts passively and transmits nothing. **Arm it only where every network in radio
   range is one you own or are explicitly authorized to test** — deauthing networks you are not
   authorized to test is illegal in most jurisdictions.
7. Save. The device stores the credentials and reboots to join the network.

On a screenless board the AP name is your only cue, which is why it is a documented, predictable
pattern rather than a random string.

### Re-provisioning

If a provisioned device cannot reach its configured network after three attempts, it falls back
to the setup portal automatically, so a moved or reconfigured appliance can be pointed at a new
network without a reflash.

The re-opened form shows your **stored settings** so a re-save does not silently undo them (ADR-0037):
the **Push these** and **Enable deauth** checkboxes come up reflecting how the device is currently set,
and the wpa-sec key and webhook URL fields carry a *"leave blank to keep"* hint. Secret values are never
displayed — only the fact that one is stored — so nothing sensitive is exposed to anyone on the setup AP.
Re-enter the **network SSID + passphrase** (the pair you came to fix), and **leave the key and webhook
blank to keep them** unchanged, or type a new value to replace one. To turn push off, uncheck all three
**Push these** boxes (you do not need to clear the URL). Switching an already-configured webhook back to
*none* through the portal is not supported (a documented limitation, [`LEDGER.md`](LEDGER.md), ADR-0037).

The setup form also has a **Maintenance dashboard passphrase** field (see below). Like the other secrets
it is accepted but never shown back, and *"leave blank to keep"* — blank on first boot means the
maintenance access point uses the default `sapper-setup` password.

### Maintenance mode (results dashboard)

To read the recovered passwords off the device without a serial console, **hold the BOOT button while
powering the device on** (ADR-0039). Instead of hunting, it comes up as a Wi-Fi access point serving a
dashboard of everything it has recovered, plus two controls (ADR-0043):

1. It raises the SoftAP **`Sapper-XXXX`** (same name as the setup portal). Join it with your
   **Maintenance dashboard passphrase** — the one you set in the setup form. If you never set one, it
   uses **`sapper-setup`**.
2. Open **`http://192.168.4.1/`** in a browser. The page lists each recovered network and **its password
   in clear**, plus how many handshakes are queued for upload, whether deauth is armed, and whether the
   device has ever synced. A board with a screen also shows the AP name, the URL, and the recovered count
   on its panel (but not the passwords — the screen is over-the-shoulder visible).
3. The dashboard has two controls at the bottom:
   - **Resume & hunt** — reboots the device back into hunting (it also syncs cracked results on its first
     window after every reboot, so there is no separate "sync now" button).
   - **Re-provision / re-arm** — opens the setup form again, pre-filled with your current toggles. Use it
     to arm or disarm deauth, flip the push selectors, or change the wpa-sec key / webhook / Maintenance
     passphrase on a device that is already deployed — those three stored secrets are never shown, so
     leaving one of *those* fields blank keeps its current value. The **Wi-Fi SSID and passphrase are the
     exception: they are always taken from the form** (not pre-filled), so to keep the same network you
     must retype both — and saving with a blank passphrase configures an *open* network. Saving reboots
     into hunting with the new configuration.
4. To leave without changing anything, **power-cycle without holding BOOT** — the device reboots and
   resumes hunting. If you walk away, it auto-resumes after **30 minutes** of no activity, so it never
   stops hunting for good.

The dashboard shows plaintext passwords deliberately: it is a local viewer on the device's own access
point, reached only by physically holding BOOT (ADR-0039). Set a strong Maintenance passphrase so only
you can join and read them — otherwise anyone in radio range who knows the default password could.

> **Secrets at rest.** Credentials are stored in plaintext NVS. Anyone with physical access and a
> flash dump can read them; flash encryption is a tracked follow-up ([`LEDGER.md`](LEDGER.md),
> ADR-0006). Treat a provisioned device as holding its network passphrase in the clear.

## Normal operation

Once provisioned, the Sapper reboots, joins the network, syncs its clock over NTP, and then **hunts
continuously with no further interaction**: it hops channels, discovers access points, captures
WPA/WPA2 4-way handshakes, and auto-uploads each new capture to wpa-sec. Once an hour it downloads
cracked results and announces any newly recovered password. It never stops on its own — a failed
upload or sync is retried, and a network it cannot reach drops it back to the setup portal after a
few attempts (see [Re-provisioning](#re-provisioning)). If **deauth is armed**, it also broadcasts
deauth frames at discovered APs to force handshakes (authorized environments only).

You can see what it is doing three ways, in increasing detail:

**Status LED** (boards with an RGB LED). The colour blinks as a slow heartbeat — proof of life — and a
hard fault shows solid:

| Colour | Meaning |
|---|---|
| Green (heartbeat) | Hunting — normal steady state |
| Blue | Working — an upload/sync window is open (briefly off-air) |
| Amber | Degraded — the network was unreachable; uploads and the hourly sync are stalled until it returns |
| Red (solid) | Fault — the radio could not resume promiscuous mode |
| Cyan (brief flash) | A handshake was just captured |
| White (~5 s flash) | A new password was just recovered |

**Screen** (boards with a panel). The HUD shows the current target and its Beacon/M1–M4 handshake
progress while hunting, a banner when a handshake is captured, and a toast when a password is
recovered. It **never** shows a recovered password — the panel is over-the-shoulder visible, so read
the passwords in [Maintenance mode](#maintenance-mode-results-dashboard) instead.

**Serial log** (USB-CDC, 115200 baud) — always available, on any board, screen or not. Key lines:
`[HUNT]` (discovery/targeting), `[CAPTURE]` (a handshake landed), `[UPLOAD]` (wpa-sec upload result),
`[SYNC]` (the hourly cracked-results sync), `[CRACK]` (a newly recovered password), `[DEAUTH]` (only
when armed), and `[WEBHOOK]` (a push was sent). `[ERROR]`/`[FATAL]` mark faults. The read-only
`ping`/`state`/`dump` commands (see [Serial control channel](#serial-control-channel-observation))
work at any time without disturbing the hunt.

## Flashing a board

The firmware is built and flashed with [PlatformIO](https://platformio.org). The only board
profile that ships today is the reference **M5Stack Cardputer ADV** (ESP32-S3, no PSRAM), built
as the `cardputer` environment; broadening to other boards (CYD, M5Stick, bare ESP32) is a tracked
follow-up ([`LEDGER.md`](LEDGER.md), slice-8). The display driver (LovyanGFX) auto-detects the
panel, so a screen is optional — a screenless board runs the same binary.

1. Connect the board over USB-C. The Cardputer's ESP32-S3 exposes a **native USB-CDC** serial port
   (no external UART bridge), so it enumerates directly.
2. Build and flash the shipped firmware:

   ```
   pio run -e cardputer -t upload
   ```

   If the board does not auto-reset into its bootloader, hold **BOOT (GPIO0)** while tapping
   **RESET**, then re-run the upload.
3. Watch the serial log at **115200 baud** (the observation channel below speaks here):

   ```
   pio run -e cardputer -t monitor
   ```

After flashing, a device with no saved network comes up in the **captive setup portal** — continue
with [Provisioning](#provisioning-first-boot) above. The `cardputer_testhooks` environment is the
bench-verify build only (it carries the serial *stimulus* vocabulary and the compile-time probes);
never deploy it, and see [Building & verifying](#building--verifying) for what each verify needs.

## Serial control channel (observation)

Every shipped build exposes a small, **read-only** serial control channel over the same USB-CDC
port (115200 baud, line-oriented, `\n`-terminated — a bare `\r` is ignored, so a CRLF terminal and
a bare-LF script behave alike). Its purpose is diagnosis and the automated verify scripts; it is
the *observation* half of the channel (ADR-0003), and by design it **mutates no engine state**
(§4 invariant #1). The *actuation/stimulus* half (injecting handshakes, forcing a sync) exists
**only** in `SAPPER_TEST_HOOKS` builds and is absent from every shipped binary (§4 invariant #4,
ADR-0047) — a shipped device cannot be commanded to upload, sync, or deauth over serial.

Matching is **exact and lower-case** (the caller is a script, not a person); leading and trailing
spaces are ignored. A line longer than **32 characters** is *refused, not truncated* — truncating a
longer word into a shorter valid one would run a command nobody asked for (ADR-0003 #5). Replies
are prefixed `[CMD]` / `[STATE]` / `[DUMP]`; a refusal is a `[CMD] refused …` line, never
`[ERROR]`/`[FATAL]` (those prefixes are reserved for real faults, so a verify can tell a rejected
command from a broken device).

| Command | Reply | Meaning |
|---|---|---|
| `ping` | `[CMD] pong` | Liveness. |
| `state` | `[STATE] phase=<phase> heap_free=<bytes> heap_max=<bytes> disp=<w>x<h>` | Observable state — boot phase, free heap, largest free block, panel geometry. Two consecutive `state`s report the same free heap, evidencing the path allocates nothing. |
| `dump` | `[DUMP] begin w=<w> h=<h> bpp=16 bytes=<n>`, then hex lines (32 bytes → 64 hex chars each), then `[DUMP] end` | Stream the framebuffer back so a verify can render it to a PNG. |
| *(blank line)* | *(nothing)* | Not an error — a bare newline is how a terminal user probes. |
| *(anything else)* | `[CMD] refused unknown-command` | Unrecognised line. |
| *(> 32 chars)* | `[CMD] refused line-too-long` | Over-length line, refused whole. |

The `phase` field on `[STATE]` is one of `provisioning`, `station_connect`, `time_sync`, `ready`,
or `maintenance` (a corrupted enum reads `invalid`). A just-flashed device announces itself with a
`[STATE]` line at boot, and every pre-engine blocking phase (e.g. the captive portal) keeps pumping
this channel, so a device is observable from the first second even before it is provisioned
(ADR-0003 #7).

## Building & verifying

This repo's `package.json` is the verification-script registry (ADR-0004), not a web toolchain.

| Command | What it does |
|---|---|
| `npm run test:native` | Host unit tests — the pure logic (ADR-0004 lane 1). |
| `npm run build:boards` | Compile the reference `cardputer` firmware (lane 2). |
| `npm run verify:device` | Serial-driven boot-face verify on an attached board (slice-0005/0042, lane 3). Proves **both** faces by which build is flashed: a normal `cardputer` build dumps the product **splash** (name + version + tagline); a `cardputer_testhooks` build with `SAPPER_TEST_PANEL=1` dumps the **panel proof** (the RGB/border/diagonal diagnostic — colour order, offset, mirror/rotation; ADR-0002 §5). PNG to `artifacts/0005-cardputer-bringup.png`. See the script header. |
| `npm run verify:provisioning` | Serial-driven provisioning verify on an attached board (slice-0007, lane 3). See the script header for the two scenarios and their preconditions. |
| `npm run verify:rf-sniffer` | Serial-driven RF sniffer verify on an attached board (slice-0012, lane 3). Flash the `cardputer_testhooks` build with `SAPPER_TEST_RF_BSSID`/`SAPPER_TEST_RF_CHANNEL` set to a nearby AP; see the script header. |
| `npm run verify:rf-discover` | Serial-driven RF channel-hopping + AP-discovery verify on an attached board (slice-0014, lane 3). Flash the `cardputer_testhooks` build with `SAPPER_TEST_RF_HOP` set to a channel list (e.g. `1,6,11`) near live APs; see the script header. |
| `npm run verify:hunt` | Serial-driven HuntEngine loop verify on an attached board (slice-0016, lane 3). Flash the `cardputer_testhooks` build with `SAPPER_TEST_HUNT` set to a channel list (e.g. `1,6,11`) near live APs; proves the discover→select→capture→discover cycle. See the script header. |
| `npm run verify:upload` | Serial-driven wpa-sec upload verify on an attached board (slice-0018, lane 3). Flash the `cardputer_testhooks` build with real `SAPPER_TEST_WIFI_SSID`/`SAPPER_TEST_WIFI_PASS`/`SAPPER_TEST_WPASEC_KEY` and `SAPPER_TEST_UPLOAD=1`; proves the TLS connection validates against the pinned GTS Root R4, the pcap POSTs, the response parses, and the hunt resumes. See the script header. |
| `npm run verify:sync` | Serial-driven wpa-sec cracked-sync verify on an attached board (slice-0020, lane 3). Flash the `cardputer_testhooks` build with the same real credentials and `SAPPER_TEST_SYNC=1`; proves the pinned-TLS `GET /?api&dl=1` download de-chunks, parses with its malformed count, seeds the LittleFS manifest, and the hunt resumes. See the script header. |
| `npm run verify:led` | Serial-driven status-LED verify on an attached board (slice-0022, lane 3). Flash the `cardputer_testhooks` build with the same real credentials and `SAPPER_TEST_LED=1`; proves the event bus drives the LED through working/hunting/recovered over the real spine. Confirm the physical colours (blue / green heartbeat / white flash) against the artifact. See the script header. |
| `npm run verify:webhook` | Serial-driven push-webhook verify on an attached board (slice-0024, lane 3). Flash the `cardputer_testhooks` build with the same real credentials, a real `SAPPER_TEST_WEBHOOK_URL` (an https ntfy/Discord endpoint), and `SAPPER_TEST_WEBHOOK=1`; proves a new-password fact travels the bus into an STA window and the transport POSTs it to the live endpoint (2xx) over TLS validated against the system CA bundle. Confirm the notification actually arrived. See the script header. |
| `npm run verify:screen` | Serial-driven status-HUD/toast verify on an attached board with a panel (slice-0026, lane 3). Flash the `cardputer_testhooks` build with `SAPPER_TEST_SCREEN=1` (no network or credentials needed — rendering is local); dumps the canvas to `artifacts/0026-screen-toast.png`. Fails if the panel is blank; confirm the HUD (status, counters, sync line) and the CRACKED banner render legibly against the artifact. See the script header. |
| `npm run verify:deauth` | Serial-driven deauth-TX verify on an attached board (slice-0028, lane 3). **Authorized targets only.** Flash the `cardputer_testhooks` build with `SAPPER_TEST_DEAUTH_BSSID` + `SAPPER_TEST_DEAUTH_CHANNEL` set to a network you own; proves the device puts real deauth/disassoc frames on air (the SDK bypass is active and `txOk>0`). Logs to `artifacts/0028-deauth-tx.txt`; if a client on the target reconnects during the run, the forced handshake is saved as `artifacts/0028-deauth-forced-handshake.pcap`. See the script header. |
| `npm run verify:deauth-loop` | Serial-driven autonomous-deauth verify on an attached board (slice-0030, lane 3). **Authorized environments only.** Drives the **shipped** `cardputer` binary, provisioned via the portal with **Enable deauth** checked and an authorized AP on air; proves an armed *shipped* loop transmits real deauth during its hunt (`[DEAUTH] ARMED`, repeating `txOk>0`). Logs to `artifacts/0030-deauth-loop-tx.txt`. See the script header. |
| `npm run verify:capture-notify` | Serial-driven capture-notification verify on an attached board with a panel (slice-0032, lane 3). Flash the `cardputer_testhooks` build with `SAPPER_TEST_CAPTURE=1` (no network or credentials needed — a capture is announced at enqueue); proves a real board emits a repeating `[CAPTURE] essid=…` line and dumps the panel to `artifacts/0032-capture-notify.png`. Confirm the CAPTURED banner names the injected network against the artifact. See the script header. |
| `npm run verify:hunt-hud` | Serial-driven live-hunt-HUD verify on an attached board with a panel (slice-0034, lane 3). Flash the `cardputer_testhooks` build with `SAPPER_TEST_HUNT_HUD=1` (no network needed), on a quiet channel; injects a synthetic AP + handshake through the engine's `onFrame` seam so the panel shows the live-hunt line, and dumps it to `artifacts/0034-hunt-hud.png`. Confirm the target + Beacon/M1/M2 indicators + progress bar render against the artifact. See the script header. |
| `npm run verify:webhook-capture` | Serial-driven capture-push webhook verify on an attached board (slice-0036, lane 3). Flash the `cardputer_testhooks` build with the same real credentials, a real `SAPPER_TEST_WEBHOOK_URL`, and `SAPPER_TEST_WEBHOOK_CAPTURE=1`; forces capture-push on, injects a synthetic capture fact, and proves the transport POSTs a "captured" notification (ESSID/BSSID, no PSK) to the live endpoint (2xx). Confirm it arrived. See the script header. |
| `npm run verify:maintenance` | Serial-driven Maintenance-mode verify on an attached board (slice-0040 + slice-0044, lane 3). Flash the `cardputer_testhooks` build with real credentials, `SAPPER_TEST_MAINT=1`, and `SAPPER_TEST_MAINT_PASS`; asserts the device enters `phase=maintenance`, prints the `[MAINT]` banner, and keeps answering serial. Join the workstation to the `Sapper-XXXX` AP with that passphrase and set `SAPPER_MAINT_DASHBOARD_URL=http://192.168.4.1/` (and optionally `SAPPER_MAINT_EXPECT_PSK`) to also fetch the dashboard, check its controls, and fetch `GET /config`. Set `SAPPER_MAINT_DRIVE_RESUME=1` to also drive `POST /resume` (this reboots the board). See the script header. |
| `npm run verify:serial-stimulus` | Serial-driven stimulus verify on an attached board (slice-0048, lane 3). Flash a **normally-booting** `cardputer_testhooks` build (real `SAPPER_TEST_WIFI_SSID`/`SAPPER_TEST_WIFI_PASS`/`SAPPER_TEST_WPASEC_KEY`, **no** probe env set); once it is hunting, the script writes `inject-handshake` over serial and proves the **shipped** hunt loop injects, enqueues, and drains the capture to wpa-sec — not a bench probe. The stimulus vocabulary exists only in `SAPPER_TEST_HOOKS` builds; a shipped binary rejects it (§4 #4, `test/test_serial_command`). See the script header. |
| `npm run lint` / `npm run index` | Documentation linter and generated ADR index. |
| `npm run check:index` | Fail if `adr/INDEX.md` is stale (the `--check` half of `index`; run by the pre-push hook and CI). |
| `npm run check:artifact-privacy` | Fail if a committed verify artifact carries a real BSSID/SoftAP identifier (ADR-0049; also run by the pre-commit hook, the pre-push hook, and CI against the pushed tree). |

## Built with Stele

This repo is developed with **[Stele](https://github.com/Isonimus/stele)** (also on npm as
[`@isonimus/stele`](https://www.npmjs.com/package/@isonimus/stele)), a documentation-as-decisions
method. Every durable decision is written as an immutable **ADR** ([`adr/`](adr/)) and every feature
as a **slice** ([`slices/`](slices/)) *before* the code, shipped in the same commit and frozen once
merged — so the record of *why* is never rewritten, only superseded. Open work lives in a single
[`LEDGER.md`](LEDGER.md); [`adr/INDEX.md`](adr/INDEX.md) is generated; and a pre-commit linter keeps
the corpus honest (immutable bodies may only gain lines, citations must resolve). A local `pre-push`
hook (`ln -sf ../../.claude/hooks/pre-push .git/hooks/pre-push`) re-runs the linter and the
artifact-privacy guard against the working tree — plus the native test suite and the board build —
before each push, and GitHub Actions CI re-runs those against the *pushed* tree on the server, plus
cppcheck and CodeQL. CI is the authoritative backstop (it scans the exact pushed ref and guards PRs
from clones without the hook); the hook is the fast local mirror (ADR-0051). The conventions
live in [`CLAUDE.md`](CLAUDE.md), and the method's toolkit (`/adr`, `/slice`, `/wrap-up`, the quality
bar) is vendored under [`.claude/`](.claude/). If the layout here looks unusual, that is why — the
commit history is meant to read as a trail of decisions.

## License

MIT — see [LICENSE](LICENSE). Pentesting tooling for authorized use only; see the
[DISCLAIMER](DISCLAIMER.md) and [SECURITY](SECURITY.md) policy.
