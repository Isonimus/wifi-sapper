# WiFi Sapper

A portable, headless-first ESP-32 firmware appliance: it hunts WPA/WPA2 handshakes endlessly,
auto-uploads each capture to [wpa-sec.stanev.org](https://wpa-sec.stanev.org), and once an hour
fetches cracked results back and announces any newly recovered passwords. The capture→upload→sync
engine has no UI dependency, so the same firmware runs on hardware with or without a screen.

The project conventions, decision records, and roadmap live in
[`CLAUDE.md`](CLAUDE.md), [`adr/`](adr/), and [`LEDGER.md`](LEDGER.md). This README is a live
document (stele:ADR-0010): the sections below describe how the firmware behaves **now**.

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

> **Secrets at rest.** Credentials are stored in plaintext NVS. Anyone with physical access and a
> flash dump can read them; flash encryption is a tracked follow-up ([`LEDGER.md`](LEDGER.md),
> ADR-0006). Treat a provisioned device as holding its network passphrase in the clear.

## Building & verifying

This repo's `package.json` is the verification-script registry (ADR-0004), not a web toolchain.

| Command | What it does |
|---|---|
| `npm run test:native` | Host unit tests — the pure logic (ADR-0004 lane 1). |
| `npm run build:boards` | Compile the reference `cardputer` firmware (lane 2). |
| `npm run verify:device` | Serial-driven bring-up verify on an attached board (slice-0005, lane 3). |
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
| `npm run lint` / `npm run index` | Documentation linter and generated ADR index. |
