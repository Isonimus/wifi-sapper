#!/usr/bin/env node
/**
 * scripts/0030-deauth-loop-verify.mjs — device verify for slice-0030 (ADR-0004 lane 3, ADR-0029).
 *
 * Proves the claim invariant #15's old gating clause forbade: a SHIPPED (non-test-hooks) binary
 * transmits real deauth frames during its autonomous hunt loop. The engine's deauth *policy* is
 * host-tested (test/test_hunt_engine); this is the on-air half — that an armed shipped appliance
 * actually puts frames on the air while hunting.
 *
 * Runs the `cardputer` (shipped) binary, NOT `cardputer_testhooks`. Runs on a workstation with the
 * board attached, never in cloud CI (§4 invariant #5). Requires `npm install` (serialport).
 *
 * *** AUTHORIZED USE ONLY. *** An armed shipped appliance broadcasts deauth at every AP it discovers.
 * Arm it only where every network in range is one you own or are explicitly authorized to test. This
 * script does not enforce that — you do.
 *
 * Precondition — the device must be provisioned AND armed, once, via the captive portal:
 *   1. Flash the shipped binary:  pio run -e cardputer -t upload
 *   2. On first boot join the "Sapper-XXXX" SoftAP, open the portal, enter your WiFi + wpa-sec key,
 *      and CHECK "Enable deauth (arm)". Save & reboot. The arm flag persists in NVS across a plain
 *      firmware RE-FLASH (NVS is a separate partition), so re-uploading the binary keeps it armed.
 *      Note: re-opening the portal and saving re-enters ALL fields — leaving the box unchecked then
 *      disarms it (the form is stateless, like the webhook field), so re-check it on any re-save.
 *   3. Ensure an authorized AP is on air so the hunt enters its Capturing phase.
 *
 * Gate (Scenario J): the device logs `[DEAUTH] ARMED` and a repeating `[DEAUTH] txOk=<N>` heartbeat
 * with N>0, and no `[FATAL]`/`[ERROR]`. txOk>0 means the shipped loop built a deauth frame, the SDK
 * bypass let it through, and esp_wifi_80211_tx accepted it — on a shipped binary.
 *
 * Scenario K (opportunistic, not the gate): if a real client re-associates during the run, the loop
 * captures and (STA permitting) uploads the forced handshake — visible as the normal [HUNT]/[UPLOAD]
 * log lines. Environmental, so never pass/fail here; each stage is proven elsewhere (slice-0018/0028).
 *
 * Usage: node scripts/0030-deauth-loop-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const LOG_PATH = `${ARTIFACT_DIR}/0030-deauth-loop-tx.txt`;
// Gate on the REPEATING txOk heartbeat (printed every 2s once armed), not a boot-once line — the S3
// USB-CDC may not reset on port-open, so a one-shot line can be missed (the slice-0028 lesson).
// Generous: the hunt must sweep 1/6/11 (discover ~4s each) and enter a Capturing window before txOk
// can climb, so allow a couple of full discover→capture cycles.
const TX_TIMEOUT_MS = 60000;

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

/** A line-oriented view over the port: remembers fatal lines, every [DEAUTH] line, and the ARMED flag. */
class Channel {
  constructor(port) {
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.deauthLines = [];
    this.sawArmed = false;
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
      this.classify(line);
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  classify(line) {
    if (/^\[(FATAL|ERROR)\]/.test(line)) this.fatalLine = line;
    if (/^\[DEAUTH\]/.test(line)) this.deauthLines.push(line);
    if (/^\[DEAUTH\] ARMED/.test(line)) this.sawArmed = true;
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
  console.log(`opened ${path} @ ${BAUD} — deauth-loop verify (SHIPPED binary)`);

  try {
    // The gate: a repeating heartbeat reporting at least one successful transmit. A heartbeat is only
    // emitted when armed, and txOk only climbs while the loop is Capturing a discovered AP, so a
    // txOk>0 line proves the shipped, armed loop transmitted deauth on air.
    const tx = await channel
      .waitForLine(/^\[DEAUTH\] txOk=([1-9]\d*)/, TX_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `no [DEAUTH] txOk>0 heartbeat within ${TX_TIMEOUT_MS}ms (${e.message}). Check: the device ` +
            `was provisioned with "Enable deauth" CHECKED (else it logs "[DEAUTH] disarmed"); an ` +
            `authorized AP is on air so the hunt enters Capturing; and watch \`pio device monitor\` ` +
            `right after reset for a boot [FATAL]. txOk stuck at 0 means the SDK bypass is not active.`,
        );
        return null;
      });

    if (tx) {
      console.log(`  ${tx}`);
      if (!channel.sawArmed) {
        // The heartbeat is armed-only, so txOk>0 implies armed; warn (not fail) if the boot line was
        // simply missed (S3 USB-CDC late attach) so the artifact still records a real pass.
        console.log('  note: [DEAUTH] ARMED boot line not seen (likely missed on late port attach)');
      }
    }

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    writeFileSync(LOG_PATH, `${channel.deauthLines.join('\n')}\n`);
    console.log(`wrote ${LOG_PATH}`);
    port.close();
  }

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`\nPASS — a SHIPPED binary transmitted real deauth frames during its hunt loop; review ${LOG_PATH}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
