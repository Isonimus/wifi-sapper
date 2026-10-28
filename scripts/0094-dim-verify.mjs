#!/usr/bin/env node
/**
 * scripts/0094-dim-verify.mjs — device verify for slice-0094 (ADR-0004 lane 3, ADR-0093).
 *
 * Proves the runtime claim a unit test cannot: on real hardware the backlight actually steps through its
 * inactivity stages AND the hunt loop keeps running while the screen is dark (the §4 backlight-only
 * invariant). The stage policy (boundaries, hysteresis-free transitions, wake rules) is lane 1
 * (test/test_screen_sleep); this is the on-air half — the controller is really wired into the shipped loop
 * and the panel really dims.
 *
 * The machine-checkable half asserts `[DIM] stage=dimmed` then `[DIM] stage=off` appear with no
 * [FATAL]/[ERROR], and that serial output continues AFTER the off transition (the loop did not pause: a
 * CPU sleep would freeze the log). The physical dimming is the photographic/eyes-on half.
 *
 * Precondition — provision (via the portal or a testhooks build with real WiFi) with SHORT delays so the
 * stages land inside the observe window, e.g. Screen dim after = 5, Screen off after = 10, on a board with
 * a panel (the M5StickC Plus2). The device must reach the hunt loop (associate).
 *
 * This script observes the running device (it never resets it).
 *
 * Usage: node scripts/0094-dim-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0094-dim.txt`;
// Association + NTP + the configured idle delays take time; allow a generous window to see dim then off.
const OBSERVE_TIMEOUT_MS = 120000;
// After the off transition, require at least one more serial line within this long to prove the loop runs.
const LIVENESS_WINDOW_MS = 15000;

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
 * A line-oriented view over the port. Remembers any fatal line, the [DIM] lines (the artifact, which is
 * identifier-free so it is safe to commit, §4 #25), the stages seen, and whether any line arrived after
 * the off transition (proof the loop kept running).
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.sawDimmed = false;
    this.sawOff = false;
    this.offAtMs = 0;
    this.lineAfterOff = false;
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
      if (/^\[(FATAL|ERROR)\]/.test(line)) this.fatalLine = this.fatalLine ?? line;
      if (/^\[DIM\]/.test(line)) this.artifact.push(line);
      if (/^\[DIM\] stage=dimmed/.test(line)) this.sawDimmed = true;
      if (/^\[DIM\] stage=off/.test(line)) {
        this.sawOff = true;
        this.offAtMs = Date.now();
      }
      // Any non-DIM line after the off transition proves loop() kept running (the backlight-only claim).
      if (this.sawOff && !/^\[DIM\]/.test(line) && Date.now() > this.offAtMs) this.lineAfterOff = true;
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  waitFor(done, timeoutMs) {
    return new Promise((resolve, reject) => {
      if (this.portError) return reject(this.portError);
      const settleIfDone = () => {
        if (done()) {
          clearTimeout(timer);
          this.remove(waiter);
          resolve(true);
          return true;
        }
        return false;
      };
      const timer = setTimeout(() => {
        this.remove(waiter);
        reject(new Error('timed out'));
      }, timeoutMs);
      const waiter = {
        onLine: () => settleIfDone(),
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
  console.log(`opened ${path} @ ${BAUD} — screen-dim verify`);

  try {
    console.log('observing the hunt loop for [DIM] stage transitions (the board is not reset)...');
    await channel
      .waitFor(() => channel.sawOff || channel.fatalLine !== null, OBSERVE_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `did not see [DIM] stage=off in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); provision short delays ` +
            `(e.g. dim=5 off=10) on a panel board and let it associate and hunt`,
        );
      });

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
    if (channel.sawOff && !channel.sawDimmed) {
      fail('saw stage=off but never stage=dimmed — the readable dim stage was skipped');
    }

    // The loop-alive proof: after off, serial output must continue (a CPU sleep would freeze it).
    if (channel.sawOff && failures.length === 0) {
      console.log('off reached; confirming the loop keeps logging (backlight-only, not a CPU sleep)...');
      await channel
        .waitFor(() => channel.lineAfterOff || channel.fatalLine !== null, LIVENESS_WINDOW_MS)
        .catch(() => {});
      if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
      if (!channel.lineAfterOff) {
        fail(`no serial output in ${LIVENESS_WINDOW_MS}ms after stage=off — the loop may have paused`);
      }
    }
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
  console.log(
    `\nPASS — the backlight stepped Active -> dimmed -> off and the hunt loop kept logging in the dark; ` +
      `confirm the panel physically dims then darkens then wakes on a capture/button (${ARTIFACT_PATH}).`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
