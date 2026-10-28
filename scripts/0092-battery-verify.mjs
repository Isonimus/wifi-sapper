#!/usr/bin/env node
/**
 * scripts/0092-battery-verify.mjs — device verify for slice-0092 (ADR-0004 lane 3, ADR-0091).
 *
 * Proves the runtime claim a unit test cannot: on real hardware the battery ADC is actually read through
 * the BatteryGauge seam, scaled by the board's measured divider, and bucketed by the pure BatteryModel.
 * The model itself (scaling, buckets, hysteresis, Unknown fail-safe) is lane 1 (test/test_battery_model,
 * test/test_screen_toast_surface); this is the on-air half: the Esp32BatteryGauge really reads the pin and
 * the hunt loop really emits the diagnostic.
 *
 * The machine-checkable half asserts a `[BATT] mv=<cell> level=<name>` line appears reporting a plausible
 * Li-ion cell voltage with no [FATAL]/[ERROR]; the human half is confirming the icon fill on the panel
 * against a multimeter reading of the cell (the photographic proof, §4 #22). Runs on a workstation with
 * the board attached, never in cloud CI (§4 invariant #5). Requires `npm install` (serialport).
 *
 * The `[BATT]` line is emitted by the normal hunt loop (no probe build needed): the device must reach the
 * hunt loop, i.e. be provisioned and associate. Precondition — flash m5stickc_plus2_testhooks with real
 * WiFi credentials (so it enters station + hunt), on a board with a measured divider (the Stick):
 *
 *   SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... pio run -e m5stickc_plus2_testhooks -t upload
 *
 * This script observes the running device (it never resets it).
 *
 * Usage: node scripts/0092-battery-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0092-battery.txt`;
// Association + NTP take time; allow a generous window to join the running loop and see a [BATT] line.
const OBSERVE_TIMEOUT_MS = 90000;

// A plausible single-cell Li-ion range, in cell millivolts. Wide enough not to false-fail across the
// charge range or on a bench-charging unit, tight enough to catch a divider applied wrong (a raw ~2000 mV
// pin reading, or a doubly-scaled ~8000 mV) — the failure that silently misreports charge.
const MIN_CELL_MV = 2500;
const MAX_CELL_MV = 4500;
const KNOWN_LEVELS = new Set(['FULL', 'HIGH', 'MID', 'LOW', 'CRITICAL']);

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
 * A line-oriented view over the port. Remembers any fatal line, every [BATT]/[SCREEN] line (the artifact,
 * identifier-free so it is safe to commit, §4 #25), and the first parsed battery reading.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.reading = null;  // { mv, level } from the first "[BATT] mv=<n> level=<name>" line.
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
      if (/^\[(BATT|SCREEN)\]/.test(line)) this.artifact.push(line);
      const m = line.match(/^\[BATT\] mv=(\d+) level=(\w+)/);
      if (m && this.reading === null) this.reading = { mv: Number(m[1]), level: m[2] };
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
  console.log(`opened ${path} @ ${BAUD} — battery verify`);

  try {
    console.log('observing the running hunt loop for a [BATT] line (the board is not reset)...');
    // A [BATT] reading OR a device fatal/error ends the wait.
    await channel
      .waitFor(() => channel.reading !== null || channel.fatalLine !== null, OBSERVE_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `saw no [BATT] mv= line in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); flash ` +
            `m5stickc_plus2_testhooks with real WiFi credentials so the device associates and hunts`,
        );
      });

    // A device-reported fatal/error fails the run — the machine-checkable half (§3).
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);

    if (channel.reading !== null) {
      const { mv, level } = channel.reading;
      if (mv < MIN_CELL_MV || mv > MAX_CELL_MV) {
        fail(`battery cell voltage ${mv} mV is outside the plausible ${MIN_CELL_MV}-${MAX_CELL_MV} mV range`);
      }
      if (!KNOWN_LEVELS.has(level)) {
        fail(`battery level "${level}" is not a known charge bucket (divider likely uncalibrated)`);
      }
      if (failures.length === 0) console.log(`  [BATT] mv=${mv} level=${level}`);
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
    `\nPASS — the hunt loop read the battery over the real seam and reported a plausible cell voltage; ` +
      `confirm the icon fill on the panel against a multimeter reading of the cell (${ARTIFACT_PATH}).`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
