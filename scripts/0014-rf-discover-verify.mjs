#!/usr/bin/env node
/**
 * scripts/0014-rf-discover-verify.mjs — device verify for slice-0014 (ADR-0004 lane 3, ADR-0013).
 *
 * Proves the two runtime claims a unit test cannot: the ESP32 sniffer, retuned by the pure hopper
 * across a channel list, (1) actually hops — the radio visits more than one channel — and (2) feeds
 * real beacons into the pure ApRegistry, which enumerates the APs in range. The hop schedule and the
 * enumeration logic are lane 1 (test/test_channel_hopper, test/test_ap_registry); this is the on-air
 * half, proving RadioSniffer::setChannel and the whole path together. Runs on a workstation with the
 * board attached, never in cloud CI (§4 invariant #5). Requires `npm install` (serialport).
 *
 * Precondition: flash the `cardputer_testhooks` build with a channel list to sweep, near live APs —
 *
 *   SAPPER_TEST_RF_HOP=1,6,11 pio run -e cardputer_testhooks -t upload
 *   (optional: SAPPER_TEST_RF_DWELL_MS=300 — the per-channel dwell; default 300)
 *
 * The probe then owns the device (it skips the normal boot) and streams [HOP]/[DISCOVER] lines. This
 * script asserts the device announces the sweep, hops across at least two distinct channels, and
 * discovers at least one AP within the timeout. The artifact records per-channel reception at the
 * configured dwell, which doubles as the dwell measurement (ADR-0013 decision #5).
 *
 * Passive receive only — this verify never asks the device to transmit (no deauth): it just listens.
 *
 * Usage: node scripts/0014-rf-discover-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0014-rf-discover.sweep.txt`;
const SWEEP_TIMEOUT_MS = 8000; // the probe prints [HOP] sweeping almost immediately after boot
const DISCOVER_TIMEOUT_MS = 45000; // allow a couple of full sweeps to hop and hear beacons
const MIN_DISTINCT_CHANNELS = 2; // proves the radio actually hopped, not sat on one channel
const MIN_DISCOVERED_APS = 1; // proves real beacons reached the pure registry

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
 * A line-oriented view over the port. Remembers any fatal line, every [HOP]/[DISCOVER] line (the
 * artifact), the set of distinct channels the radio hopped to, and the discovered APs. Waiters are
 * rejected together on a transport error so a disconnect fails loud at once.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.hopChannels = new Set();
    this.discoveredAps = [];
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
      if (/^\[(HOP|DISCOVER)\]/.test(line)) this.artifact.push(line);
      const hop = line.match(/^\[HOP\] channel=(\d+)/);
      if (hop) this.hopChannels.add(Number(hop[1]));
      if (/^\[DISCOVER\] ap bssid=/.test(line)) this.discoveredAps.push(line);
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  /** Resolve with the first line matching `pattern`; reject on timeout or transport error. */
  waitForLine(pattern, timeoutMs) {
    let matched = null;
    return this.waitFor(() => matched, timeoutMs, (line) => {
      if (matched === null && pattern.test(line)) matched = line;
    });
  }

  /**
   * Resolve once `done()` returns truthy — checked after each line and once up front. `observe` is
   * called for every line so callers can accumulate state. Rejects on timeout or transport error.
   */
  waitFor(done, timeoutMs, observe = () => {}) {
    return new Promise((resolve, reject) => {
      if (this.portError) return reject(this.portError);
      const settleIfDone = () => {
        const value = done();
        if (value) {
          clearTimeout(timer);
          this.remove(waiter);
          resolve(value);
          return true;
        }
        return false;
      };
      const timer = setTimeout(() => {
        this.remove(waiter);
        reject(new Error('timed out'));
      }, timeoutMs);
      const waiter = {
        onLine: (line) => {
          observe(line);
          settleIfDone();
        },
        reject: (err) => {
          clearTimeout(timer);
          reject(err);
        },
      };
      this.waiters.push(waiter);
      if (settleIfDone()) return;
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
  console.log(`opened ${path} @ ${BAUD} — RF discover verify`);

  try {
    // The probe must be active. If [HOP] sweeping never comes, the build likely has no channel list
    // set (SAPPER_TEST_RF_HOP empty) so it booted normally — a precondition error, reported loud.
    const sweeping = await channel
      .waitForLine(/^\[HOP\] sweeping /, SWEEP_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `device never announced [HOP] sweeping (${e.message}); flash cardputer_testhooks with ` +
            `SAPPER_TEST_RF_HOP set to a channel list (e.g. 1,6,11)`,
        );
        return null;
      });

    if (sweeping) {
      console.log(`  ${sweeping}`);
      await channel
        .waitFor(
          () =>
            channel.hopChannels.size >= MIN_DISTINCT_CHANNELS &&
            channel.discoveredAps.length >= MIN_DISCOVERED_APS,
          DISCOVER_TIMEOUT_MS,
        )
        .catch((e) => {
          fail(
            `sweep did not both hop across ${MIN_DISTINCT_CHANNELS}+ channels and discover ` +
              `${MIN_DISCOVERED_APS}+ AP within ${DISCOVER_TIMEOUT_MS}ms (${e.message}); saw ` +
              `${channel.hopChannels.size} distinct channel(s) and ${channel.discoveredAps.length} ` +
              `AP(s) — check APs are powered and in range on the swept channels`,
          );
        });
      console.log(
        `  hopped ${channel.hopChannels.size} channel(s), discovered ${channel.discoveredAps.length} AP(s)`,
      );
      for (const ap of channel.discoveredAps) console.log(`  ${ap}`);
    }

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    writeFileSync(ARTIFACT_PATH, `${channel.artifact.join('\n')}\n`);
    console.log(`wrote ${ARTIFACT_PATH}`);
    port.close();
  }

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`\nPASS — the sweep hopped across channels and discovered real APs; review ${ARTIFACT_PATH}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
