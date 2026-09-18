#!/usr/bin/env node
/**
 * scripts/0016-hunt-verify.mjs — device verify for slice-0016 (ADR-0004 lane 3, ADR-0015).
 *
 * Proves the runtime claim a unit test cannot: the HuntEngine, on real hardware, actually runs its
 * endless loop — it discovers real APs, selects a target and retunes to it (entering capture), and
 * cycles back to discovery. The state-machine logic (transitions, round-robin, the retune-failure
 * skip, the quiesce settle) is lane 1 (test/test_hunt_engine); this is the on-air half, proving the
 * atomic-router consumer switching and the whole path together. Runs on a workstation with the board
 * attached, never in cloud CI (§4 invariant #5). Requires `npm install` (serialport).
 *
 * Precondition: flash the `cardputer_testhooks` build with a hop channel list, near live APs —
 *
 *   SAPPER_TEST_HUNT=1,6,11 pio run -e cardputer_testhooks -t upload
 *   (optional: SAPPER_TEST_HUNT_DWELL_MS=300 — the per-channel dwell; default 300)
 *
 * The probe then owns the device (it skips the normal boot) and streams [HUNT] lines. This script
 * observes the RUNNING loop — it never resets the board, because this board's native USB-CDC
 * re-enumerates on reset (as verify:provisioning documents) — and asserts, from wherever it joins,
 * that the device discovers real targets, enters capture, and cycles back to discovery (a discovery
 * announcement seen after a capture proves the loop closed). No live handshake is asserted —
 * capturing a real 4-way handshake needs a deauth to force renegotiation and a pcap sink to write it
 * (slice-5, LEDGER). The artifact records the observed [HUNT] lines and phase timing (the
 * quiesce/settle measurement — ADR-0015 decision #4).
 *
 * Passive receive only — this verify never asks the device to transmit (no deauth): it just listens.
 * It may take up to a couple of minutes to join a running loop and witness a full cycle, especially
 * in a dense environment where each discovered AP gets its own capture window before re-discovery.
 *
 * Usage: node scripts/0016-hunt-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0016-hunt.loop.txt`;
// Long enough to join a running loop and witness a full discover→capture→discover cycle: each
// discovered AP gets its own capture window before re-discovery, so a dense environment takes minutes.
const OBSERVE_TIMEOUT_MS = 150000;
const MIN_TARGETS = 1; // proves real beacons reached the pure registry through the engine
const MIN_CAPTURES = 1; // proves the engine selected a target and entered capture

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
 * A line-oriented view over the port. Remembers any fatal line, every [HUNT] line (the artifact), and
 * counts the loop's observable events: discovery announcements, discovered targets, and captures.
 * Waiters are rejected together on a transport error so a disconnect fails loud at once.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.discoveries = 0;
    this.targets = [];
    this.captures = [];
    this.cycled = false; // a discovery announcement seen after a capture — the loop closed.
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
      if (/^\[HUNT\]/.test(line)) this.artifact.push(line);
      if (/^\[HUNT\] discovering/.test(line)) {
        this.discoveries += 1;
        if (this.captures.length > 0) this.cycled = true; // discover AFTER capture = a closed cycle.
      }
      if (/^\[HUNT\] target bssid=/.test(line)) this.targets.push(line);
      if (/^\[HUNT\] capturing bssid=/.test(line)) this.captures.push(line);
      for (const w of [...this.waiters]) w.onLine(line);
    }
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
  console.log(`opened ${path} @ ${BAUD} — hunt loop verify`);

  try {
    // Observe the running loop — no reset (the board's USB-CDC re-enumerates on reset). From wherever
    // this joins, require the engine to discover a real target, enter capture, and close a cycle (a
    // discovery announcement after a capture). If no [HUNT] line ever appears the build has no hop list
    // set (SAPPER_TEST_HUNT empty) and booted normally — a precondition error, reported loud.
    console.log('observing the running hunt loop (the board is not reset — this can take a minute)...');
    await channel
      .waitFor(
        () =>
          channel.targets.length >= MIN_TARGETS &&
          channel.captures.length >= MIN_CAPTURES &&
          channel.cycled,
        OBSERVE_TIMEOUT_MS,
      )
      .catch((e) => {
        if (channel.artifact.length === 0) {
          fail(
            `saw no [HUNT] output in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); flash cardputer_testhooks ` +
              `with SAPPER_TEST_HUNT set to a channel list (e.g. 1,6,11)`,
          );
        } else {
          fail(
            `hunt loop did not close an observable discover→capture→discover cycle within ` +
              `${OBSERVE_TIMEOUT_MS}ms (${e.message}); saw ${channel.discoveries} discovery ` +
              `announcement(s), ${channel.targets.length} target(s), ${channel.captures.length} ` +
              `capture(s), cycled=${channel.cycled} — check APs are powered and in range on the swept ` +
              `channels`,
          );
        }
      });

    if (failures.length === 0) {
      console.log(
        `  ${channel.discoveries} discovery announcement(s), ${channel.targets.length} target(s), ` +
          `${channel.captures.length} capture entry(ies), cycled=${channel.cycled}`,
      );
      for (const t of channel.targets) console.log(`  ${t}`);
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
  console.log(`\nPASS — the hunt loop discovered, selected, captured, and cycled; review ${ARTIFACT_PATH}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
