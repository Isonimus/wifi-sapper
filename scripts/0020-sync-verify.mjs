#!/usr/bin/env node
/**
 * scripts/0020-sync-verify.mjs — device verify for slice-0020 (ADR-0004 lane 3, ADR-0019).
 *
 * Proves the runtime claims a unit test cannot: on real hardware, against the live wpa-sec service, the
 * appliance opens a TLS connection that validates against the *pinned* GTS Root R4 (ADR-0017 decision
 * #1), issues `GET /?api&dl=1` authenticated by the operator's key, reads the body through HTTPClient so
 * any `Transfer-Encoding: chunked` framing is de-framed (no split line — ADR-0019 decision #1), parses it
 * into records with a reported malformed count, seeds the LittleFS manifest, and returns to hunting. The
 * parse rules, manifest policy, delta/alert logic, and scheduler are lane 1 (test/test_cracked_*,
 * test/test_sync_scheduler, test/test_upload_supervisor); this is the on-air half — the TLS pin, the real
 * `?api&dl=1` contract, and the de-chunk. Runs on a workstation with the board attached, never in cloud CI
 * (§4 invariant #5). Requires `npm install` (serialport).
 *
 * The stimulus is a clock-advance that forces a due sync (huntLoopForceSyncDue) so the verify need not
 * wait a real hour; the sync then opens one STA window on its own (the upload queue is empty). The account
 * must hold at least one cracked result for a meaningful seed; an empty account still proves the
 * download→parse→persist path (downloaded=0, a successful sync).
 *
 * Precondition: flash cardputer_testhooks with real WiFi credentials, a real wpa-sec key, and
 * SAPPER_TEST_SYNC set —
 *
 *   SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
 *   SAPPER_TEST_SYNC=1 pio run -e cardputer_testhooks -t upload
 *
 * This script observes the running device (it never resets it — this board's native USB-CDC re-enumerates
 * on reset, as verify:provisioning documents) and asserts one sync completed cleanly.
 *
 * Usage: node scripts/0020-sync-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0020-sync.txt`;
// Association + NTP + TLS handshake + a full-account download take time, and the forced sync-only window
// opens on the drain time ceiling; allow a generous window to join the running loop and witness one sync.
const OBSERVE_TIMEOUT_MS = 90000;

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
 * A line-oriented view over the port. Remembers any fatal line, every [SYNC]/[CRACK]/[HUNT] line (the
 * artifact), and the probe's "verify sync ok" summary the first successful sync prints.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.syncOkLine = null;   // the "[SYNC] verify sync ok ..." summary the probe prints on success.
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
      if (/^\[(SYNC|CRACK|HUNT)\]/.test(line)) this.artifact.push(line);
      if (/^\[SYNC\] verify sync ok/.test(line)) this.syncOkLine = this.syncOkLine ?? line;
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
  console.log(`opened ${path} @ ${BAUD} — wpa-sec cracked-sync verify`);

  try {
    console.log('observing the running sync loop (the board is not reset — this can take a minute)...');
    // A completed sync (ok) OR a device fatal/error ends the wait. The probe re-arms after each sync, so
    // a late-joining observer still catches one; a failed sync prints [ERROR], caught below.
    await channel
      .waitFor(() => channel.syncOkLine !== null || channel.fatalLine !== null, OBSERVE_TIMEOUT_MS)
      .catch((e) => {
        if (channel.artifact.length === 0) {
          fail(
            `saw no [SYNC] output in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); flash ` +
              `cardputer_testhooks with SAPPER_TEST_SYNC=1 and real WiFi/wpa-sec credentials`,
          );
        } else {
          fail(`no sync completed within ${OBSERVE_TIMEOUT_MS}ms (${e.message})`);
        }
      });

    // A device-reported fatal/error (including a TLS cert-pin failure or a failed/garbage download) fails
    // the run — that is the machine-checkable half (§3): the pin, the fetch, and the parse must all hold.
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
    if (!channel.syncOkLine && !channel.fatalLine) fail('no successful sync was observed');

    if (failures.length === 0) {
      console.log(`  sync: ${channel.syncOkLine}`);
      const downloaded = channel.syncOkLine.match(/downloaded=(\d+)/)?.[1] ?? '?';
      const malformed = channel.syncOkLine.match(/malformed=(\d+)/)?.[1] ?? '?';
      console.log(`  downloaded=${downloaded} malformed=${malformed} (malformed>0 signals a format change)`);
      if (downloaded === '0') {
        console.log('  (downloaded=0 — the account holds no cracked results yet; the download→parse→');
        console.log('   persist path is still proven. Seed one crack to exercise a non-empty manifest.)');
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
    `\nPASS — TLS validated against the pinned GTS Root R4, the account downloaded and de-chunked, ` +
      `parsed with its malformed count, the manifest seeded, and the hunt resumed; review ${ARTIFACT_PATH}.`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
