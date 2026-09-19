#!/usr/bin/env node
/**
 * scripts/0018-upload-verify.mjs — device verify for slice-0018 (ADR-0004 lane 3, ADR-0017).
 *
 * Proves the runtime claims a unit test cannot: on real hardware, against the live wpa-sec service,
 * the appliance opens a TLS connection that validates against the *pinned* GTS Root R4 (ADR-0017
 * decision #1), POSTs a serialized pcap authenticated by the operator's key, parses wpa-sec's
 * response fail-closed (decision #2), and resumes hunting. The queue policy, backoff, and the
 * delete-on-accepted/duplicate logic are lane 1 (test/test_upload_supervisor, test/test_capture_queue);
 * this is the on-air half — the TLS pin, the wire format, and the real response. Runs on a workstation
 * with the board attached, never in cloud CI (§4 invariant #5). Requires `npm install` (serialport).
 *
 * The stimulus is a *synthetic* wpa-sec-valid handshake injected through the capture-ready seam, so
 * the verify need not wait for a natural (deauth-forced) capture. A synthetic capture is not
 * crackable, so the live service classifies it `rejected` ("no valid handshakes") — which still proves
 * the whole TLS→POST→parse→resume path (a cert-pin or network failure is an [ERROR] instead). Inject a
 * real captured handshake to additionally exercise the accepted/duplicate→delete path.
 *
 * Precondition: flash cardputer_testhooks with real WiFi credentials, a real wpa-sec key, and
 * SAPPER_TEST_UPLOAD set —
 *
 *   SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
 *   SAPPER_TEST_UPLOAD=1 pio run -e cardputer_testhooks -t upload
 *
 * This script observes the running device (it never resets it — this board's native USB-CDC
 * re-enumerates on reset, as verify:provisioning documents) and asserts the drain happened cleanly.
 *
 * Usage: node scripts/0018-upload-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0018-upload.drain.txt`;
// Association + NTP + TLS handshake + POST + response take time; allow a generous window to join the
// running loop and witness one full drain.
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
 * A line-oriented view over the port. Remembers any fatal line, every [UPLOAD]/[HUNT] line (the
 * artifact), the wpa-sec result classification, and the "drain done" summary the probe prints.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.uploadResult = null; // accepted | duplicate | rejected
    this.drainLine = null;    // the "[UPLOAD] drain done ..." summary
    this.drainAssociated = false;
    this.drainResumeFailed = true;
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
      if (/^\[(UPLOAD|HUNT)\]/.test(line)) this.artifact.push(line);
      const result = line.match(/^\[UPLOAD\] wpa-sec result=(accepted|duplicate|rejected)/);
      if (result) this.uploadResult = result[1];
      if (/^\[UPLOAD\] drain done/.test(line)) {
        this.drainLine = line;
        this.drainAssociated = /associated=1/.test(line);
        this.drainResumeFailed = /resumeFailed=1/.test(line);
      }
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
  console.log(`opened ${path} @ ${BAUD} — wpa-sec upload verify`);

  try {
    console.log('observing the running upload loop (the board is not reset — this can take a minute)...');
    await channel
      .waitFor(() => channel.drainLine !== null || channel.fatalLine !== null, OBSERVE_TIMEOUT_MS)
      .catch((e) => {
        if (channel.artifact.length === 0) {
          fail(
            `saw no [UPLOAD] output in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); flash ` +
              `cardputer_testhooks with SAPPER_TEST_UPLOAD=1 and real WiFi/wpa-sec credentials`,
          );
        } else {
          fail(`no drain completed within ${OBSERVE_TIMEOUT_MS}ms (${e.message})`);
        }
      });

    // A device-reported fatal/error (including a TLS cert-pin failure) fails the run.
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);

    if (channel.drainLine) {
      if (!channel.drainAssociated) fail(`drain did not associate: ${channel.drainLine}`);
      if (channel.drainResumeFailed) fail(`hunt did not resume after the drain: ${channel.drainLine}`);
      if (channel.uploadResult === null) {
        fail('a drain ran but no wpa-sec response was parsed (no [UPLOAD] wpa-sec result= line)');
      }
    }

    if (failures.length === 0) {
      console.log(`  drain: ${channel.drainLine}`);
      console.log(`  wpa-sec classified the upload as: ${channel.uploadResult}`);
      if (channel.uploadResult === 'rejected') {
        console.log('  (rejected is expected for the synthetic stimulus — the TLS pin + POST + parse');
        console.log('   path is proven; inject a real handshake to exercise accepted/duplicate→delete)');
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
    `\nPASS — TLS validated against the pinned GTS Root R4, the pcap POSTed, the response parsed, ` +
      `and the hunt resumed; review ${ARTIFACT_PATH}.`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
