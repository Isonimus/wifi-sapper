#!/usr/bin/env node
/**
 * scripts/0058-keyboard-probe.mjs — on-device keycode probe for slice-0058 (ADR-0057, lane 3).
 *
 * A probe, not a wired regression verify (docs/quality-bar §3: a probe answers a measurable question
 * once and the number lands in the ADR). It confirms the fact ADR-0057 decision 5 defers to hardware:
 * that the Cardputer ADV's `m` key decodes to key number 48, the constant `isMaintenanceKey` matches.
 *
 * It drives a board flashed with the keyboard probe (SAPPER_TEST_KB=1), prompts the operator to press
 * `m`, and reads the `[KB] code=N ...` line the firmware logs for that press. It PASSES if the pressed
 * key both reports the expected keycode and is flagged as the Maintenance key; it FAILS on any [FATAL]
 * line, on no keypress within the window, or on a keycode mismatch (which means the constant in
 * `tca8418_event.h` and ADR-0057 must be amended to the measured value).
 *
 * Interactive and hardware-attached — never cloud CI (ADR-0004 invariant #5). Needs no network.
 * Port: argv[2], else $SAPPER_PORT, else auto-detect.
 *
 * Usage: node scripts/0058-keyboard-probe.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';

const BAUD = 115200;
const EXPECTED_KEYCODE = 48; // kMaintenanceKeycode in src/hal/input/tca8418_event.h (the `m` key).
const PROBE_TIMEOUT_MS = 15000; // the probe boots and brings up the TCA8418, then prints [KB] probe up
const PRESS_TIMEOUT_MS = 30000; // how long to wait for the operator to press `m`

const failures = [];
const fail = (message) => failures.push(message);

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

/** A line-oriented view over the port, remembering any fatal line seen at any time. */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.portError = null;
    port.on('data', (chunk) => this.onData(chunk));
    port.on('error', (err) => (this.portError = err));
  }

  onData(chunk) {
    this.buffer += chunk.toString('utf8');
    let nl;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl).replace(/\r$/, '');
      this.buffer = this.buffer.slice(nl + 1);
      if (/^\[FATAL\]/.test(line)) this.fatalLine = line;
      for (const w of [...this.waiters]) {
        if (w.pattern.test(line)) {
          this.waiters = this.waiters.filter((x) => x !== w);
          w.resolve(line);
        }
      }
    }
  }

  waitForLine(pattern, timeoutMs) {
    return new Promise((resolve, reject) => {
      const waiter = { pattern, resolve };
      this.waiters.push(waiter);
      setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w !== waiter);
        reject(new Error(`timed out waiting for ${pattern} after ${timeoutMs}ms`));
      }, timeoutMs);
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
  console.log(`opened ${path} @ ${BAUD}`);

  try {
    await channel.waitForLine(/^\[KB\] probe up/, PROBE_TIMEOUT_MS);
    console.log(`\n>>> Press the 'm' key on the Cardputer now (waiting ${PRESS_TIMEOUT_MS / 1000}s)...\n`);

    // Wait for the first PRESS event (bit7 set → pressed=1); releases (pressed=0) are ignored.
    const line = await channel.waitForLine(/^\[KB\] raw=0x[0-9A-Fa-f]+ code=\d+ pressed=1/, PRESS_TIMEOUT_MS);
    console.log(`observed: ${line}`);
    const m = /code=(\d+)/.exec(line);
    const keycode = m ? Number(m[1]) : NaN;

    if (keycode !== EXPECTED_KEYCODE) {
      fail(
        `pressed key reported code=${keycode}, expected ${EXPECTED_KEYCODE} — if you pressed 'm', ` +
          `update kMaintenanceKeycode in src/hal/input/tca8418_event.h and amend ADR-0057 to ${keycode}`,
      );
    }
    if (!/<- MAINTENANCE KEY/.test(line)) {
      fail(`the pressed key was not flagged as the Maintenance key (isMaintenanceKey did not match)`);
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
  console.log(`\nPASS — the 'm' key decodes to keycode ${EXPECTED_KEYCODE} and matches isMaintenanceKey.`);
}

main().catch((err) => {
  console.error(`\nERROR: ${err.message}`);
  process.exit(1);
});
