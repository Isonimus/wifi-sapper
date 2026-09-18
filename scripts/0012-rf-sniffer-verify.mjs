#!/usr/bin/env node
/**
 * scripts/0012-rf-sniffer-verify.mjs — device verify for slice-0012 (ADR-0004 lane 3, ADR-0011).
 *
 * Proves the one runtime claim a unit test cannot: the ESP32 promiscuous seam, on a fixed channel,
 * receives real 802.11 frames and routes them through the real consumer into the pure capture core.
 * The seam wiring itself is lane 1 (test/test_radio_sniffer); this is the on-air half. Runs on a
 * workstation with the board attached, never in cloud CI (§4 invariant #5). Requires `npm install`
 * (serialport).
 *
 * Precondition: flash the `cardputer_testhooks` build with the RF target set to a known access
 * point that is powered on nearby, on a known channel —
 *
 *   SAPPER_TEST_RF_BSSID=aa:bb:cc:dd:ee:ff SAPPER_TEST_RF_CHANNEL=6 \
 *     pio run -e cardputer_testhooks -t upload
 *
 * The probe then owns the device (it skips the normal boot) and streams `[SNIFF]` lines. This script
 * asserts the device announces it is listening and, within the timeout, reports the target AP's
 * beacon reaching the core. A beacon is emitted continuously by any AP, so no client join is needed;
 * capturing a full 4-way handshake belongs to a later slice with an emit path (ADR-0011).
 *
 * Passive receive only — this verify never asks the device to transmit (no deauth): it just listens.
 *
 * Usage: node scripts/0012-rf-sniffer-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0012-rf-sniffer.sniff.txt`;
const LISTEN_TIMEOUT_MS = 8000; // the probe prints [SNIFF] listening almost immediately after boot
const BEACON_TIMEOUT_MS = 30000; // an in-range AP beacons many times a second; allow for distance

const failures = [];
const fail = (message) => failures.push(message);

/** Resolve the serial port: explicit arg/env wins, else the first Espressif-looking device. */
async function resolvePort() {
  const explicit = process.argv[2] || process.env.SAPPER_PORT;
  if (explicit) return explicit;
  const ports = await SerialPort.list();
  const match = ports.find(
    (p) => /espressif/i.test(p.manufacturer ?? '') || /ttyACM|usbmodem/.test(p.path),
  );
  if (!match) throw new Error('no serial port given and none auto-detected (set $SAPPER_PORT)');
  return match.path;
}

/**
 * A line-oriented view over the port, remembering any fatal line and every [SNIFF] line seen (the
 * artifact). Waiters are rejected together on a transport error so a disconnect fails loud at once.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.sniffLines = [];
    this.portError = null;
    port.on('data', (chunk) => this.onData(chunk));
    port.on('error', (err) => this.onError(err));
  }

  remove(waiter) {
    this.waiters = this.waiters.filter((w) => w !== waiter);
  }

  onError(err) {
    this.portError = err;
    const pending = this.waiters;
    this.waiters = [];
    for (const w of pending) w.reject(err);
  }

  onData(chunk) {
    this.buffer += chunk.toString('utf8');
    let nl;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl).replace(/\r$/, '');
      this.buffer = this.buffer.slice(nl + 1);
      if (/^\[(FATAL|ERROR)\]/.test(line)) this.fatalLine = line;
      if (/^\[SNIFF\]/.test(line)) this.sniffLines.push(line);
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  /** Resolve with the first line matching `pattern`; reject on timeout or transport error. */
  waitForLine(pattern, timeoutMs) {
    return new Promise((resolve, reject) => {
      if (this.portError) {
        reject(this.portError);
        return;
      }
      const timer = setTimeout(() => {
        this.remove(waiter);
        reject(new Error(`timed out waiting for ${pattern}`));
      }, timeoutMs);
      const waiter = {
        onLine: (line) => {
          if (!pattern.test(line)) return;
          clearTimeout(timer);
          this.remove(waiter);
          resolve(line);
        },
        reject: (err) => {
          clearTimeout(timer);
          reject(err);
        },
      };
      this.waiters.push(waiter);
    });
  }
}

async function main() {
  const path = await resolvePort();
  const port = new SerialPort({ path, baudRate: BAUD });
  await new Promise((resolve, reject) => {
    port.once('open', resolve);
    port.once('error', reject);
  });
  const channel = new Channel(port);
  mkdirSync(ARTIFACT_DIR, { recursive: true });
  console.log(`opened ${path} @ ${BAUD} — RF sniffer verify`);

  try {
    // The probe must be active. If [SNIFF] listening never comes, the build likely has no RF target
    // set (SAPPER_TEST_RF_BSSID empty) so it booted normally — a precondition error, reported loud.
    const listening = await channel
      .waitForLine(/^\[SNIFF\] listening channel=/, LISTEN_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `device never announced [SNIFF] listening (${e.message}); flash cardputer_testhooks with ` +
            `SAPPER_TEST_RF_BSSID and SAPPER_TEST_RF_CHANNEL set to a nearby AP`,
        );
        return null;
      });

    if (listening) {
      console.log(`  ${listening}`);
      const beacon = await channel
        .waitForLine(/^\[SNIFF\] beacon bssid=/, BEACON_TIMEOUT_MS)
        .catch((e) => {
          fail(
            `target beacon did not reach the core within ${BEACON_TIMEOUT_MS}ms (${e.message}); ` +
              `check the AP is powered, in range, and on SAPPER_TEST_RF_CHANNEL`,
          );
          return null;
        });
      if (beacon) console.log(`  ${beacon}`);
    }

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    writeFileSync(ARTIFACT_PATH, `${channel.sniffLines.join('\n')}\n`);
    console.log(`wrote ${ARTIFACT_PATH}`);
    port.close();
  }

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`\nPASS — the promiscuous seam received the target beacon on air; review ${ARTIFACT_PATH}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
