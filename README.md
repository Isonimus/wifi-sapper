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
5. Save. The device stores the credentials and reboots to join the network.

On a screenless board the AP name is your only cue, which is why it is a documented, predictable
pattern rather than a random string.

### Re-provisioning

If a provisioned device cannot reach its configured network after three attempts, it falls back
to the setup portal automatically, so a moved or reconfigured appliance can be pointed at a new
network without a reflash.

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
| `npm run lint` / `npm run index` | Documentation linter and generated ADR index. |
