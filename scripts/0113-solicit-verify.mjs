#!/usr/bin/env node
/**
 * scripts/0113-solicit-verify.mjs — device verify for slice-0113 (ADR-0004 lane 3, ADR-0110 #5d).
 *
 * Proves clause (b)/(c) of §4 #47 on air: a SHIPPED (non-test-hooks) binary injects the blind
 * clientless-PMKID solicitation pair (open-auth, then an association request with a CCMP+PSK RSN IE)
 * during its autonomous hunt loop when the operator armed solicitation. The engine's solicit *policy*
 * (the two-phase cadence, stop-on-PMKID, the two decoupled arms) is host-tested (test/test_hunt_engine,
 * slice-0112); this is the on-air half — that an armed shipped appliance actually puts the auth/assoc
 * frames on the air while hunting.
 *
 * Runs a SHIPPED binary (e.g. `m5stickc_plus2` or `cardputer`), NOT a `*_testhooks` build. Runs on a
 * workstation with the board attached, never in cloud CI (§4 invariant #5). Requires `npm install`.
 *
 * *** AUTHORIZED USE ONLY. *** An armed shipped appliance solicits a PMKID from every PMK-caching AP it
 * captures, impersonating a station with a spoofed MAC. Arm it only where every network in range is one
 * you own or are explicitly authorized to test. This script does not enforce that — you do.
 *
 * Precondition — the device must be provisioned AND solicit-armed, once, via the captive portal:
 *   1. Flash the shipped binary:  pio run -e m5stickc_plus2 -t upload
 *   2. On first boot join the "Sapper-XXXX" SoftAP, open the portal, enter your WiFi + wpa-sec key,
 *      and CHECK "Enable PMKID solicitation (arm)". Save & reboot. The arm flag persists in NVS across
 *      a plain firmware RE-FLASH (NVS is a separate partition), so re-uploading the binary keeps it
 *      armed. Note: re-opening the portal and saving re-enters ALL fields — leaving the box unchecked
 *      then disarms it (the form renders stored state, so re-check it only if you meant to keep it on).
 *   3. Ensure an authorized PMK-caching AP is on air so the hunt enters its Capturing phase.
 *
 * Gate: the device logs a repeating `[SOLICIT] txOk=<N>` heartbeat with N>0, and no `[FATAL]`/`[ERROR]`.
 * txOk>0 means the shipped loop built an auth or assoc frame, the SDK raw-TX bypass let it through, and
 * esp_wifi_80211_tx accepted it — on a shipped binary, during its hunt.
 *
 * Opportunistic (not the gate): if an armed AP answers with an M1 carrying a PMKID KDE, the loop
 * captures and (STA permitting) uploads it — visible as the normal [CAPTURE]/[UPLOAD] log lines. A
 * solicited PMKID is environmental (the AP must PMK-cache and answer), so it is never pass/fail here;
 * each downstream stage is proven elsewhere (slice-0018 upload, slice-0112 the unified drain).
 *
 * Usage: node scripts/0113-solicit-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const LOG_PATH = `${ARTIFACT_DIR}/0113-solicit-loop-tx.txt`;
// Gate on the REPEATING txOk heartbeat (printed every 2s once armed), not a boot-once line — the S3
// USB-CDC may not reset on port-open, so a one-shot line can be missed (the slice-0028 lesson, the
// deauth-loop verify's basis too). Generous: the hunt must sweep 1-13 (discover ~4s each) and enter a
// Capturing window before the first auth is sent and txOk can climb, so allow a couple of full cycles.
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

/** A line-oriented view over the port: remembers fatal lines, every [SOLICIT] line, and the ARMED flag. */
class Channel {
  constructor(port) {
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.solicitLines = [];
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
    if (/^\[SOLICIT\]/.test(line)) this.solicitLines.push(line);
    if (/^\[SOLICIT\] ARMED/.test(line)) this.sawArmed = true;
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
  console.log(`opened ${path} @ ${BAUD} — solicit-loop verify (SHIPPED binary)`);

  try {
    // The gate: a repeating heartbeat reporting at least one successful transmit. A heartbeat is only
    // emitted when armed, and txOk only climbs while the loop is Capturing a discovered AP, so a
    // txOk>0 line proves the shipped, armed loop injected the auth/assoc solicit pair on air.
    const tx = await channel
      .waitForLine(/^\[SOLICIT\] txOk=([1-9]\d*)/, TX_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `no [SOLICIT] txOk>0 heartbeat within ${TX_TIMEOUT_MS}ms (${e.message}). Check: the device ` +
            `was provisioned with "Enable PMKID solicitation" CHECKED (else it logs "[SOLICIT] ` +
            `disarmed"); an authorized AP is on air so the hunt enters Capturing; and watch ` +
            `\`pio device monitor\` right after reset for a boot [FATAL]. txOk stuck at 0 means the ` +
            `SDK raw-TX bypass is not active.`,
        );
        return null;
      });

    if (tx) {
      console.log(`  ${tx}`);
      if (!channel.sawArmed) {
        // The heartbeat is armed-only, so txOk>0 implies armed; warn (not fail) if the boot line was
        // simply missed (S3 USB-CDC late attach) so the artifact still records a real pass.
        console.log('  note: [SOLICIT] ARMED boot line not seen (likely missed on late port attach)');
      }
    }

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    writeFileSync(LOG_PATH, `${channel.solicitLines.join('\n')}\n`);
    console.log(`wrote ${LOG_PATH}`);
    port.close();
  }

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`\nPASS — a SHIPPED binary injected clientless-PMKID solicitation during its hunt loop; review ${LOG_PATH}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
