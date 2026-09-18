#!/usr/bin/env node
/**
 * scripts/0007-provisioning-verify.mjs — device verify for slice-0007 (ADR-0004 lane 3).
 *
 * Proves the two runtime provisioning claims a unit test cannot (the pure gate/validation/naming
 * is lane 1, test/test_provisioning). Runs on a workstation with the board attached, never in
 * cloud CI (ADR-0004 invariant #5). Requires `npm install` (serialport).
 *
 * Two scenarios, selected by $SAPPER_VERIFY_SCENARIO (default 'c'):
 *
 *   c — UNATTENDED, machine-checkable. Precondition: NVS erased (esptool erase_flash, NOT over
 *       serial — invariant #7) and the plain `cardputer` build flashed. Asserts the device is in
 *       the portal (`state` -> `[STATE] phase=provisioning`) AND still answers `ping`/`state`
 *       while the portal blocks — that last part is the regression for §4 invariant #3 (a
 *       pre-engine blocking state pumps serial). Active queries, so it does not depend on catching
 *       the boot instant; the boot-time `[PORTAL]` banner is captured as evidence when present.
 *       Needs no access point.
 *
 *   d — ATTENDED, bench. Precondition: the `cardputer_testhooks` build flashed, built with
 *       $SAPPER_TEST_WIFI_SSID/$SAPPER_TEST_WIFI_PASS/$SAPPER_TEST_WPASEC_KEY for a reachable
 *       network. Launch this, then reset the board; asserts the announced phases advance
 *       provisioning->station_connect->time_sync->ready and a final `state` reads `phase=ready`.
 *
 * Both halves fail loud on any [FATAL]/[ERROR] line and write an artifact for review.
 *
 * Usage: SAPPER_VERIFY_SCENARIO=c node scripts/0007-provisioning-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_STATE = `${ARTIFACT_DIR}/0007-provisioning.state.txt`;
const REPLY_TIMEOUT_MS = 4000;
const PHASE_TIMEOUT_MS = 45000; // STA association + first NTP sync can take tens of seconds

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
 * A line-oriented view over the port, remembering any fatal line seen at any time. Waiters are
 * `{ onLine, reject }`; a transport error rejects every pending waiter at once so a disconnect
 * fails loud immediately rather than stalling each read to its timeout.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.portalLine = null;
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
      if (/^\[PORTAL\] ssid=/.test(line)) this.portalLine = line; // boot-time evidence, if caught
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  send(command) {
    return new Promise((resolve, reject) =>
      this.port.write(`${command}\n`, (err) => (err ? reject(err) : resolve())),
    );
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

/**
 * Scenario C: an unprovisioned device raises the portal and keeps serving serial while it blocks.
 * Machine-checkable and needs no access point.
 */
async function verifyPortal(channel) {
  const observed = [];

  // The regression for invariant #3: with the portal blocking in loop(), the channel must still
  // answer. If loop() failed to pump serial during provisioning these two would time out.
  const pongP = channel.waitForLine(/^\[CMD\] pong/, REPLY_TIMEOUT_MS);
  await channel.send('ping');
  await pongP.catch((e) => fail(`portal did not answer ping (invariant #3): ${e.message}`));

  const stateP = channel.waitForLine(/^\[STATE\] phase=provisioning\b/, REPLY_TIMEOUT_MS);
  await channel.send('state');
  const state = await stateP.catch((e) => {
    fail(`state during portal not phase=provisioning: ${e.message}`);
    return null;
  });
  if (state) observed.push(state);

  // Boot-time evidence: present only if this run caught the boot (e.g. the operator reset the
  // board). Not required — the SoftAP naming is unit-tested (lane 1) and the checks above are the
  // machine-checkable core.
  if (channel.portalLine) observed.push(channel.portalLine);

  writeFileSync(ARTIFACT_STATE, `${observed.join('\n')}\n`);
  console.log(`wrote ${ARTIFACT_STATE}`);
}

/**
 * Scenario D: a provisioned device advances through the phases to Ready. Attended — the operator
 * resets the board after launch so the announced progression streams from a clean boot.
 */
async function verifyStationBoot(channel) {
  console.log('reset the board now — waiting for the boot phase progression...');
  const observed = [];

  // The phases are announced in order as they happen; wait for each in turn from the reset boot.
  for (const [phase, timeout] of [
    ['station_connect', PHASE_TIMEOUT_MS],
    ['time_sync', PHASE_TIMEOUT_MS],
    ['ready', PHASE_TIMEOUT_MS],
  ]) {
    const line = await channel
      .waitForLine(new RegExp(`^\\[STATE\\] phase=${phase}\\b`), timeout)
      .catch((e) => {
        fail(`did not reach phase=${phase}: ${e.message}`);
        return null;
      });
    if (line) observed.push(line);
  }

  // Confirm the steady state with an explicit query, independent of catching the announcement.
  const stateP = channel.waitForLine(/^\[STATE\] phase=ready\b/, REPLY_TIMEOUT_MS);
  await channel.send('state');
  await stateP.catch((e) => fail(`final state not phase=ready: ${e.message}`));

  writeFileSync(ARTIFACT_STATE, `${observed.join('\n')}\n`);
  console.log(`wrote ${ARTIFACT_STATE}`);
}

async function main() {
  const scenario = (process.env.SAPPER_VERIFY_SCENARIO ?? 'c').toLowerCase();
  if (scenario !== 'c' && scenario !== 'd') {
    throw new Error(`unknown scenario '${scenario}' (expected 'c' or 'd')`);
  }

  const path = await resolvePort();
  const port = new SerialPort({ path, baudRate: BAUD });
  await new Promise((resolve, reject) => {
    port.once('open', resolve);
    port.once('error', reject);
  });
  const channel = new Channel(port);
  mkdirSync(ARTIFACT_DIR, { recursive: true });
  console.log(`opened ${path} @ ${BAUD} — scenario ${scenario}`);

  try {
    if (scenario === 'c') {
      await verifyPortal(channel);
    } else {
      await verifyStationBoot(channel);
    }
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    port.close();
  }

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`\nPASS — scenario ${scenario} verified; review ${ARTIFACT_STATE}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
