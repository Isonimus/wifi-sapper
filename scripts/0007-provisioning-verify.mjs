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
 *   d — bench. Precondition: the `cardputer_testhooks` build flashed, built with
 *       $SAPPER_TEST_WIFI_SSID/$SAPPER_TEST_WIFI_PASS/$SAPPER_TEST_WPASEC_KEY for a reachable
 *       network. Polls `state` and records the phase sequence as the device advances
 *       station_connect->time_sync->ready; passes once it reads `phase=ready` (reachable only
 *       after association and an NTP sync past the 2020 sentinel). No board reset needed — and it
 *       must not be reset mid-run: this board's native USB-CDC serial re-enumerates on reset,
 *       which would drop the port. Just run it against the booting or booted device.
 *
 * Both scenarios fail loud on any [FATAL]/[ERROR] line and write an artifact for review.
 *
 * Usage: SAPPER_VERIFY_SCENARIO=c node scripts/0007-provisioning-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
// Per-scenario artifact: C and D run against different builds/NVS states, so each keeps its own
// evidence rather than overwriting the other's (ADR-0004 invariant #5).
const artifactPath = (scenario) => `${ARTIFACT_DIR}/0007-provisioning.scenario-${scenario}.state.txt`;
const REPLY_TIMEOUT_MS = 4000;
const PHASE_TIMEOUT_MS = 45000; // STA association + first NTP sync can take tens of seconds

const POLL_INTERVAL_MS = 2000; // between `state` polls while the device advances through phases

const failures = [];
const fail = (message) => failures.push(message);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

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
async function verifyPortal(channel, artifact) {
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

  writeFileSync(artifact, `${observed.join('\n')}\n`);
  console.log(`wrote ${artifact}`);
}

/**
 * Scenario D: a provisioned device advances through the phases to Ready. Polls `state` rather
 * than catching a boot stream, because this board's native USB-CDC serial does not survive a
 * reset (the port re-enumerates). The device answers `state` in every phase (invariant #3), so
 * polling records the progression as it happens and confirms the end state without a reset.
 */
async function verifyStationBoot(channel, artifact) {
  console.log('polling state as the device advances to ready (do not reset the board)...');
  const sequence = []; // one entry per distinct phase observed, in order

  const deadline = Date.now() + PHASE_TIMEOUT_MS;
  let reachedReady = false;
  while (Date.now() < deadline) {
    // A spontaneous enterPhase() announcement and a `state` reply share the [STATE] line format, so
    // this waiter may resolve on either. That is harmless: [STATE] always carries the device's live
    // phase, so whichever line wins reports the true current phase — no stale/false reading (freeze
    // adversarial review, dismissed with this reason so it is not re-raised).
    const stateP = channel.waitForLine(/^\[STATE\] phase=\w+/, REPLY_TIMEOUT_MS);
    await channel.send('state');
    const line = await stateP.catch(() => null);
    if (line) {
      const phase = /phase=(\w+)/.exec(line)?.[1] ?? null;
      if (phase && (sequence.length === 0 || sequence[sequence.length - 1].phase !== phase)) {
        sequence.push({ phase, line });
        console.log(`  observed phase=${phase}`);
      }
      if (phase === 'ready') {
        reachedReady = true;
        break;
      }
    }
    if (channel.fatalLine) break; // fail-loud handled by the caller
    await sleep(POLL_INTERVAL_MS);
  }

  if (!reachedReady) {
    const last = sequence[sequence.length - 1]?.line ?? 'no reply';
    fail(`did not reach phase=ready within ${PHASE_TIMEOUT_MS}ms (last: ${last})`);
  }

  writeFileSync(artifact, `${sequence.map((s) => s.line).join('\n')}\n`);
  console.log(`wrote ${artifact}`);
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
  const artifact = artifactPath(scenario);
  console.log(`opened ${path} @ ${BAUD} — scenario ${scenario}`);

  try {
    if (scenario === 'c') {
      await verifyPortal(channel, artifact);
    } else {
      await verifyStationBoot(channel, artifact);
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
  console.log(`\nPASS — scenario ${scenario} verified; review ${artifact}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
