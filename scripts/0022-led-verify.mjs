#!/usr/bin/env node
/**
 * scripts/0022-led-verify.mjs — device verify for slice-0022 (ADR-0004 lane 3, ADR-0021).
 *
 * Proves the runtime claim a unit test cannot: on real hardware the status LED is actually driven
 * through its states over the real hunt→drain→sync spine. The LED *policy* (which fact lights which
 * status, the heartbeat, the flash) is lane 1 (test/test_led_status_surface, test/test_event_bus);
 * this is the on-air half — the bus is really wired into the shipped loop, the Esp32 driver really
 * runs, and the WS2812 lights. The machine-checkable half asserts the driver logged all three driven
 * states — working (an STA window opened), hunting (it returned to the heartbeat), and recovered (a
 * new-password flash); the human half is confirming the physical colours on the board (blue, green
 * heartbeat, white flash) against the committed artifact.
 *
 * The stimulus is the LED probe: it forces STA windows (working → hunting) and injects a synthetic
 * new-password fact through the bus (recovered), re-driving both so a late-joining monitor still sees
 * every state. Runs on a workstation with the board attached, never in cloud CI (§4 invariant #5).
 * Requires `npm install` (serialport).
 *
 * Precondition: flash cardputer_testhooks with real WiFi credentials, a real wpa-sec key, and
 * SAPPER_TEST_LED set —
 *
 *   SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
 *   SAPPER_TEST_LED=1 pio run -e cardputer_testhooks -t upload
 *
 * This script observes the running device (it never resets it — this board's native USB-CDC
 * re-enumerates on reset, as verify:provisioning documents).
 *
 * Usage: node scripts/0022-led-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0022-led.txt`;
// Association + NTP + one STA window (opened on the drain time ceiling) take time; allow a generous
// window to join the running loop and witness all three driven LED states.
const OBSERVE_TIMEOUT_MS = 90000;

// The states the driver must be seen to reach for the run to pass.
const REQUIRED = ['working', 'hunting', 'recovered'];

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
 * A line-oriented view over the port. Remembers any fatal line, every [LED]/[SYNC]/[HUNT] line (the
 * artifact), and the set of LED statuses the driver has reported.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.seen = new Set();  // LED statuses observed, from "[LED] status=<name>" lines.
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
      if (/^\[(LED|SYNC|HUNT)\]/.test(line)) this.artifact.push(line);
      const status = line.match(/^\[LED\] status=(\w+)/)?.[1];
      if (status) this.seen.add(status);
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  haveAllRequired() {
    return REQUIRED.every((s) => this.seen.has(s));
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
  console.log(`opened ${path} @ ${BAUD} — status-LED verify`);

  try {
    console.log('observing the running LED loop (the board is not reset — this can take a minute)...');
    // All three required states seen OR a device fatal/error ends the wait. The probe re-drives every
    // state, so a late-joining observer still catches them all; a fault prints [FATAL]/[ERROR].
    await channel
      .waitFor(() => channel.haveAllRequired() || channel.fatalLine !== null, OBSERVE_TIMEOUT_MS)
      .catch((e) => {
        if (channel.seen.size === 0) {
          fail(
            `saw no [LED] output in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); flash ` +
              `cardputer_testhooks with SAPPER_TEST_LED=1 and real WiFi/wpa-sec credentials`,
          );
        } else {
          const missing = REQUIRED.filter((s) => !channel.seen.has(s));
          fail(`LED did not reach all states within ${OBSERVE_TIMEOUT_MS}ms (missing: ${missing.join(', ')})`);
        }
      });

    // A device-reported fatal/error fails the run — the machine-checkable half (§3): the LED path must
    // come up cleanly over the real spine.
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);

    if (failures.length === 0) {
      console.log(`  LED reached: ${[...channel.seen].join(', ')}`);
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
    `\nPASS — the event bus drove the status LED through working, hunting, and recovered over the real ` +
      `spine; confirm the physical colours (blue / green heartbeat / white flash) against ${ARTIFACT_PATH}.`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
