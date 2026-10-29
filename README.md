# WiFi Sapper

<p align="center">
  <img src="docs/images/wifi-sapper-banner.webp" alt="WiFi Sapper: autonomous handshake hunter" width="880">
</p>

<p align="center">
  <strong>Autonomous, headless-first WPA/WPA2 handshake-hunting ESP-32 appliance</strong>
</p>

<p align="center">
  <a href="https://github.com/Isonimus/wifi-sapper/actions/workflows/ci.yml"><img src="https://github.com/Isonimus/wifi-sapper/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/Isonimus/wifi-sapper/actions/workflows/codeql.yml"><img src="https://github.com/Isonimus/wifi-sapper/actions/workflows/codeql.yml/badge.svg" alt="CodeQL"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/status-alpha-orange" alt="Status: alpha">
  <img src="https://img.shields.io/badge/platform-ESP32%20%2F%20ESP32--S3-333?logo=espressif" alt="Platform: ESP32 / ESP32-S3">
  <img src="https://img.shields.io/badge/built%20with-PlatformIO-orange?logo=platformio" alt="Built with PlatformIO">
  <a href="https://www.npmjs.com/package/@isonimus/stele"><img src="https://img.shields.io/badge/method-Stele-6E44FF" alt="Built with Stele"></a>
</p>

<p align="center">
  <a href="#supported-boards">Boards</a> •
  <a href="#provisioning-first-boot">Provisioning</a> •
  <a href="#normal-operation">Operation</a> •
  <a href="#flashing-a-board">Flashing</a> •
  <a href="#building--verifying">Building</a> •
  <a href="#security-model-and-known-limitations">Security</a> •
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
7. Optionally, **screen dimming** (ADR-0093), two idle delays in seconds on a board with a panel:
   **Screen dim after** (the backlight drops to a readable ~15% after this long idle; 0 = never dim) and
   **Screen off after** (fully dark after this long; 0 = dim only, never off). Both **off by default**.
   The screen brightens again on a capture or a crack (and on a button press, on button boards), and the
   hunt keeps running the whole time: dimming only changes the backlight, it never sleeps the device.
   Saves battery and reduces the lit-screen tell of a planted unit. Both fields are pre-filled with their
   stored value when you reopen the form, so a re-save to change something else preserves them.
8. Save. The device stores the credentials and reboots to join the network.

On a board **with a screen**, these join details (the `Sapper-XXXX` name, the `sapper-setup`
passphrase, and `http://192.168.4.1/`) are shown on the panel while the setup portal is up
(ADR-0089), so a device flashed from m5burner can be provisioned without ever opening this guide.
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

To read the recovered passwords off the device without a serial console, trigger Maintenance **while
the device is hunting**: the entry gesture is per board (ADR-0057, ADR-0078). On the **Cardputer ADV**
press the **`m` key** (there is no runtime button wired to GPIO0, which is a boot strap only used for
firmware flashing, so the keyboard is the entry input). On the **M5StickC Plus2**, which has no keyboard,
**hold the BtnB side button for about 2 seconds**. On the **LilyGo T-embed**, **hold the rotary-encoder
push switch for about 2 seconds** (its button is on GPIO0, which unlike the Cardputer's is wired to a
runtime-pressable switch). Any of these gestures saves a one-shot "enter Maintenance"
flag and reboots into Maintenance; there is no timing window to catch, so trigger it whenever you like.
Instead of hunting, the device then comes up as a Wi-Fi access point serving a dashboard of everything it
has recovered, plus two controls (ADR-0043):

1. It raises the SoftAP **`Sapper-Maint-XXXX`** — a distinct name from the setup portal's `Sapper-XXXX`
   (ADR-0055), so you can always tell Maintenance apart from the setup/STA-fail portal. Join it with your
   **Maintenance dashboard passphrase** — the one you set in the setup form. If you never set one, it
   uses **`sapper-setup`**.
2. Open **`http://192.168.4.1/`** in a browser. The page lists each recovered network and **its password
   in clear**, plus how many handshakes are queued for upload, whether deauth is armed, whether the
   device has ever synced, and (if its storage was ever reformatted after corruption) a warning that
   persisted data was lost (ADR-0074). A board with a screen also shows the AP name, the URL, and the recovered count
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
4. To leave without changing anything, **power-cycle the device** — it reboots and resumes hunting (the
   Maintenance flag is one-shot and a plain power-on never re-enters Maintenance). If you walk away, it
   auto-resumes after **30 minutes** of no activity, so it never stops hunting for good.

The dashboard shows plaintext passwords deliberately: it is a local viewer on the device's own access
point, reached only by a physical keypress at the device (ADR-0039, entry per ADR-0057). Set a strong
Maintenance passphrase so only
you can join and read them — otherwise anyone in radio range who knows the default password could.

> **Secrets at rest.** Credentials are stored in plaintext NVS. Anyone with physical access and a
> flash dump can read them; flash encryption is a tracked follow-up ([`LEDGER.md`](LEDGER.md),
> ADR-0006). Treat a provisioned device as holding its network passphrase in the clear. See the full
> [security model and known limitations](#security-model-and-known-limitations).

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

**Screen** (boards with a panel). The HUD title carries the product name and firmware version; below
it the HUD shows the current target and its Beacon/M1–M4 handshake progress while hunting, a banner
when a handshake is captured, and a toast when a password is recovered. A coarse **battery icon** sits
top-right on a board whose battery divider has been measured (the M5StickC Plus2 today; it reads the
cell voltage and shows a four-segment charge level), so a field unit's remaining power is visible at a
glance. The layout adapts to the panel size, so a larger panel (e.g. the T-embed's 320x170) uses the
extra room rather than a fixed corner. It **never** shows a recovered password — the panel is
over-the-shoulder visible, so read the passwords in
[Maintenance mode](#maintenance-mode-results-dashboard) instead.

**Serial log** (USB-CDC, 115200 baud) — always available, on any board, screen or not. Key lines:
`[HUNT]` (discovery/targeting), `[CAPTURE]` (a handshake landed), `[UPLOAD]` (wpa-sec upload result),
`[SYNC]` (the hourly cracked-results sync), `[CRACK]` (a newly recovered password), `[DEAUTH]` (only
when armed), and `[WEBHOOK]` (a push was sent). `[ERROR]`/`[FATAL]` mark faults. The read-only
`ping`/`state`/`dump` commands (see [Serial control channel](#serial-control-channel-observation))
work at any time without disturbing the hunt.

## Supported boards

Four board profiles ship today. The capture→upload→sync engine is identical across all of them; the
rows differ only in hardware the surfaces adapt to (panel, PSRAM, the Maintenance entry gesture). The
firmware is headless-first, so a screenless or LED-less board runs the same binary.

| Board | MCU | PSRAM | Flash | Display | Maintenance entry | PlatformIO env |
|---|---|---|---|---|---|---|
| **M5Stack Cardputer ADV** *(reference)* | ESP32-S3 | none | 8 MB | 1.14" 240x135 | press `m` key | `cardputer` |
| **M5StickC Plus2** | ESP32-PICO-V3-02 | 2 MB quad | 8 MB | 1.14" 240x135 | hold BtnB ~2 s | `m5stickc_plus2` |
| **LilyGo T-embed** | ESP32-S3 | 8 MB octal | 16 MB | 1.9" 320x170 | hold encoder push ~2 s | `t_embed` |
| **ESP32-3248S035C (CYD)** | ESP32-D0WD-V3 | none | 4 MB | 3.5" 320x480 ST7796 | hold BOOT ~2 s | `cyd_3248s035c` |

**Proof status.** The capture→upload→sync engine is identical on every board and is compile-and-link
verified in CI on each push (all four environments, both chip families). On-air verify evidence (the
committed [`artifacts/`](artifacts/)) was produced on the **Cardputer reference target**; the other
three boards have been brought up on hardware, but some of their per-board on-air proofs (photographic
HUD/panel confirmation, the M5StickC Plus2 PSRAM boot, the CYD sync leg) are **pending** and tracked
in [`LEDGER.md`](LEDGER.md). The bench procedure for the engine proofs (Scenario J, the CYD sync leg,
gate-C re-submit suppression) is [`docs/on-air-proof-runbook.md`](docs/on-air-proof-runbook.md). See
[Security model and known limitations](#security-model-and-known-limitations).

**USB serial.** The two ESP32-S3 boards (Cardputer, T-embed) expose a **native USB-CDC / USB-Serial-JTAG**
port, so they enumerate directly and do not reset when a serial monitor opens. The M5StickC Plus2 and the
CYD use a **CH340 USB-to-serial bridge**, which resets the board (via DTR/RTS) when a port opens — normal,
and how the bench verifies wake it.

**Status LED** (see the [colour table](#normal-operation)): the Cardputer drives a WS2812 RGB LED, the
M5StickC Plus2 a single red LED, and the T-embed and CYD have no LED wired yet (the T-embed's APA102 and
the CYD's 3-GPIO RGB are deferred, [`LEDGER.md`](LEDGER.md)).

The CYD is the first board that is both no-PSRAM and a classic ESP32, so its two cracked-manifest buffers
are placed in the internal DRAM heap one-at-a-time (ADR-0082) rather than static `.bss`, which overflows.
Its 3.5" 320x480 ST7796 panel is lit via a **direct-to-panel renderer** (ADR-0084): a full-frame RGB565
canvas would be 307 KB against this board's ~110 KB largest free block, so drawing goes straight to the
panel with no offscreen buffer. It is presented in **landscape (480x320)** to match the other boards. The
boot splash, the **live hunt HUD**, and the Maintenance panel all render on it: because there is no offscreen
buffer, the HUD avoids flicker by repainting only changed fields rather than clearing the whole panel each
frame (a second renderer laid out for 480x320, ADR-0086, leaving the other boards' renderer untouched).
**GT911 capacitive touch** is an additive follow-up ([`LEDGER.md`](LEDGER.md)), as is broadening to a bare
ESP32.

## Flashing a board

### Flash from your browser (no toolchain)

The easiest path: open the hosted **[web flasher](https://isonimus.github.io/wifi-sapper/)** in desktop
Chrome, Edge, or Opera, pick your board, plug it in over USB, and click *Connect & Flash*. It writes the
latest tagged release's factory image over Web Serial, with no local toolchain. Install erases the whole
device, so you will re-run first-boot [Provisioning](#provisioning-first-boot) afterwards. The page and its
per-board manifests are the `web/` tree, deployed to GitHub Pages (slices/0102).

### Install a release by file

Each tagged release also publishes, per supported board, a **factory image** (full flash, write at offset
`0x0`) and an **app image** (OTA slot), plus `SHA256SUMS.txt`, on the repo's
[Releases](https://github.com/Isonimus/wifi-sapper/releases) page. To install from a file, flash the factory
image for your board with [esptool](https://github.com/espressif/esptool):

```
esptool --chip <esp32s3|esp32> write-flash 0x0 sapper-<board>-<version>-factory.bin
```

The images are built `dio`-headed on purpose: a `qio`-headed factory image hard-bricks the chip, so the
release refuses to publish one that is not `dio`. Then continue with [Provisioning](#provisioning-first-boot).

### Build from source

The firmware is built and flashed with [PlatformIO](https://platformio.org). Pick your board's
environment from the [Supported boards](#supported-boards) table above. The display driver (LovyanGFX)
is configured per board, so a screen is optional: a screenless board runs the same binary.

1. Connect the board over USB-C (see the USB serial note under [Supported boards](#supported-boards)
   for how each board enumerates).
2. Build and flash the shipped firmware (substitute your board's env for `cardputer`):

   ```
   pio run -e cardputer -t upload          # Cardputer ADV
   pio run -e m5stickc_plus2 -t upload     # M5StickC Plus2
   pio run -e t_embed -t upload            # LilyGo T-embed
   pio run -e cyd_3248s035c -t upload      # ESP32-3248S035C (CYD), ST7796 320x480
   ```

   If the board does not auto-reset into its bootloader, hold **BOOT (GPIO0)** while tapping
   **RESET**, then re-run the upload.
3. Watch the serial log at **115200 baud** (the observation channel below speaks here):

   ```
   pio run -e cardputer -t monitor
   ```

After flashing, a device with no saved network comes up in the **captive setup portal**: continue
with [Provisioning](#provisioning-first-boot) above. Each board also has a `*_testhooks` environment
(`cardputer_testhooks`, `m5stickc_plus2_testhooks`, `t_embed_testhooks`, `cyd_3248s035c_testhooks`), the bench-verify build only (it carries the
serial *stimulus* vocabulary and the compile-time probes); never deploy it, and see
[Building & verifying](#building--verifying) for what each verify needs.

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
| `npm run verify:maintenance` | Serial-driven Maintenance-mode verify on an attached board (slice-0040 + slice-0044, lane 3). Flash the `cardputer_testhooks` build with real credentials, `SAPPER_TEST_MAINT=1`, and `SAPPER_TEST_MAINT_PASS`; asserts the device enters `phase=maintenance`, prints the `[MAINT]` banner, and keeps answering serial. Join the workstation to the `Sapper-Maint-XXXX` AP with that passphrase and set `SAPPER_MAINT_DASHBOARD_URL=http://192.168.4.1/` (and optionally `SAPPER_MAINT_EXPECT_PSK`) to also fetch the dashboard, check its controls, and fetch `GET /config`. Set `SAPPER_MAINT_DRIVE_RESUME=1` to also drive `POST /resume` (this reboots the board). See the script header. |
| `npm run verify:keyboard` | Interactive on-device keycode probe on an attached board (slice-0058, lane 3; a probe, not a wired verify). Flash the `cardputer_testhooks` build with `SAPPER_TEST_KB=1` (no network needed); it logs each keypress's decoded key number. Press `m` when prompted — it PASSES if that key reports `code=48` and matches `isMaintenanceKey`, confirming the Maintenance keycode ADR-0057 records. See the script header. |
| `npm run probe:rssi` | On-device RSSI↔capture-correlation probe on an attached board (ADR-0067, lane 3; a probe, not a wired verify). Flash the `cardputer_testhooks` build with `SAPPER_TEST_RSSI` set to a channel list (e.g. `1,6,11`) and `SAPPER_TEST_RSSI_RUN` to a run tag, near your **own authorized** APs (the hunt is armed — it transmits deauth). It runs the real hunt with **no cracked/submitted gate** (so it attempts every encrypted AP regardless of wpa-sec state — a fully-uploaded lab needs no memory wipe) and never uploads; it taps each beacon's RSSI out-of-band and accumulates a per-band capture histogram to LittleFS. Because the multi-hour run is **untethered**, the reader is used by simply connecting to the running board (no reset needed — this board's native USB re-enumerates on reset, so the histogram is dumped periodically, not only at boot): it captures the next identity-free `[RSSI] histogram …` block, writes the correlation table to `artifacts/0068-rssi-correlation.txt`, then tails live rows for a tethered smoke test until Ctrl-C or `--minutes N`. A power-cycle resumes the same run tag; bumping the tag + reflashing starts clean. The evidence decides whether RSSI-first ordering is worth widening the §4 #8 seam. See the script header. |
| `npm run verify:serial-stimulus` | Serial-driven stimulus verify on an attached board (slice-0048, lane 3). Flash a **normally-booting** `cardputer_testhooks` build (real `SAPPER_TEST_WIFI_SSID`/`SAPPER_TEST_WIFI_PASS`/`SAPPER_TEST_WPASEC_KEY`, **no** probe env set); once it is hunting, the script writes `inject-handshake` then `force-sync` over serial and proves the **shipped** hunt loop injects, enqueues, and drains the capture to wpa-sec — not a bench probe — reading the `[UPLOAD] drain done` summary the shipped-loop logger prints in a hooks build (ADR-0103). The stimulus vocabulary exists only in `SAPPER_TEST_HOOKS` builds; a shipped binary rejects it (§4 #4, `test/test_serial_command`). See the script header. |
| `npm run lint` / `npm run index` | Documentation linter and generated ADR index. |
| `npm run check:index` | Fail if `adr/INDEX.md` is stale (the `--check` half of `index`; run by the pre-push hook and CI). |
| `npm run check:artifact-privacy` | Fail if a committed verify artifact carries a real BSSID/SoftAP identifier (ADR-0049; also run by the pre-commit hook, the pre-push hook, and CI against the pushed tree). |

## Security model and known limitations

The Sapper is offensive tooling: handshake capture and operator-armed deauth are its intended
function. Its security *design* is about protecting the operator and the operator's secrets. In brief:
provisioned secrets are never echoed to the served setup page, no plaintext password leaves the device
over the network or appears on the panel/LED/serial log, the wpa-sec cloud link is certificate-pinned,
and provisioning is never writable over serial (the enforcing invariants are in
[`docs/security-model.md`](docs/security-model.md)).

Three relaxations are deliberate and recorded: the Maintenance dashboard serves recovered passwords in
clear over its own passphrase-gated access point (set a strong Maintenance passphrase); the raw-deauth
capability ships in every binary but transmits only when the operator arms it (off by default); and
the setup access point uses the public default password `sapper-setup`.

Known limitations to weigh before a field deployment:

- **Secrets at rest are plaintext NVS.** A flash dump reveals the WiFi passphrase and wpa-sec key;
  flash encryption is a tracked follow-up (ADR-0006).
- **The wpa-sec CA pin expires 2036-06-22 and fails closed.** A firmware release updates the pinned
  certificate (ADR-0017).
- **Self-hosted webhooks behind a private CA are unsupported.** Use a publicly-trusted certificate
  (ADR-0023).
- **A green verify does not yet catch every ESP-IDF fault.** An assurance gap being hardened
  (ADR-0074).

The full threat model, the deliberate exceptions, and the per-board on-air proof status are in
[`docs/security-model.md`](docs/security-model.md). To report a firmware security *defect*, see the
[`SECURITY.md`](SECURITY.md) policy.

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
