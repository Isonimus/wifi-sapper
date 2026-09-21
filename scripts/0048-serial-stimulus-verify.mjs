#!/usr/bin/env node
/**
 * scripts/0048-serial-stimulus-verify.mjs — device verify for slice-0048 (ADR-0004 lane 3, ADR-0047).
 *
 * Proves the runtime claim a unit test cannot: the SAPPER_TEST_HOOKS stimulus vocabulary, sent over
 * serial, drives the *shipped* hunt loop — not a bench probe. The board is flashed with a plain
 * cardputer_testhooks build (real WiFi + wpa-sec credentials, NO probe env set), so it boots normally
 * into runStationBoot → huntLoopBegin and hunts. This script then writes `inject-handshake\n` to the
 * port and asserts the loop drains the injected synthetic capture to wpa-sec and resumes hunting.
 *
 * The parser-side guarantee — that this vocabulary is ABSENT from a shipped (non-hooks) build — is the
 * §4 #4 invariant and is proven on the always-run native lane (test/test_serial_command); this on-air
 * half proves the gated dispatch reaches the engine through the real capture-ready seam (§4 #2).
 *
 * A synthetic capture is not crackable, so the live service classifies it `rejected` — which still
 * proves the whole serial→inject→enqueue→drain→TLS→POST→parse→resume path (a cert-pin, network, or
 * wiring failure is an [ERROR]/[FATAL] instead). Runs on a workstation with the board attached, never
 * in cloud CI (§4 invariant #5). Requires `npm install` (serialport).
 *
 * Precondition: flash a NORMALLY-BOOTING hooks build (no SAPPER_TEST_* probe var set) —
 *
 *   SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
 *   pio run -e cardputer_testhooks -t upload
 *
 * Usage: node scripts/0048-serial-stimulus-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0048-serial-stimulus.txt`;
// The device must first reach Ready (STA associate + NTP) before the stimulus can drain: give it a
// generous window to be hunting, then a second window for the injected capture to drain.
const READY_TIMEOUT_MS = 90000;
const DRAIN_TIMEOUT_MS = 90000;

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
 * A line-oriented view over the port. Remembers any fatal line, the [HUNT] "loop started" marker, the
 * [CMD] ack for the injected command, and the [UPLOAD] drain summary the supervisor prints.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.hunting = false;       // saw the shipped loop announce itself.
    this.injectAcked = false;   // saw the [CMD] inject-handshake ack.
    this.drainLine = null;      // the "[UPLOAD] drain done ..." summary.
    this.drainAssociated = false;
    this.drainResumeFailed = true;
    this.uploadResult = null;   // accepted | duplicate | rejected
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
      if (/^\[(HUNT|UPLOAD|CMD|CAPTURE)\]/.test(line)) this.artifact.push(line);
      if (/^\[HUNT\] discovering/.test(line)) this.hunting = true;
      if (/^\[CMD\] inject-handshake/.test(line)) this.injectAcked = true;
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

const write = (port, text) =>
  new Promise((resolve, reject) => port.write(text, (err) => (err ? reject(err) : resolve())));

async function main() {
  const path = await resolvePort();
  const port = new SerialPort({ path, baudRate: BAUD });
  await new Promise((resolve, reject) => {
    port.once('open', resolve);
    port.once('error', reject);
  });
  const channel = new Channel(port);
  mkdirSync(ARTIFACT_DIR, { recursive: true });
  console.log(`opened ${path} @ ${BAUD} — serial stimulus verify`);

  try {
    // 1. Wait for the shipped hunt loop to be running (it must reach Ready before a stimulus can drain).
    console.log('waiting for the shipped hunt loop to start (STA associate + NTP; can take a minute)...');
    await channel
      .waitFor(() => channel.hunting || channel.fatalLine !== null, READY_TIMEOUT_MS)
      .catch((e) =>
        fail(
          `device did not reach the hunt loop in ${READY_TIMEOUT_MS}ms (${e.message}); flash a ` +
            `NORMALLY-BOOTING cardputer_testhooks build with real WiFi/wpa-sec credentials and no probe env`,
        ),
      );
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);

    // 2. Drive the *shipped* loop over serial — the point of this verify (ADR-0047).
    if (channel.hunting && !channel.fatalLine) {
      console.log('hunt loop is up — sending `inject-handshake` over serial...');
      await write(port, 'inject-handshake\n');
      await channel
        .waitFor(() => channel.drainLine !== null || channel.fatalLine !== null, DRAIN_TIMEOUT_MS)
        .catch((e) => fail(`no drain completed within ${DRAIN_TIMEOUT_MS}ms of the stimulus (${e.message})`));
    }

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
    if (!channel.injectAcked && !channel.fatalLine) {
      fail('device did not ack the stimulus ([CMD] inject-handshake) — is this a SAPPER_TEST_HOOKS build?');
    }
    if (channel.drainLine) {
      if (!channel.drainAssociated) fail(`drain did not associate: ${channel.drainLine}`);
      if (channel.drainResumeFailed) fail(`hunt did not resume after the drain: ${channel.drainLine}`);
      if (channel.uploadResult === null) {
        fail('a drain ran but no wpa-sec response was parsed (no [UPLOAD] wpa-sec result= line)');
      }
    }

    if (failures.length === 0) {
      console.log(`  drain: ${channel.drainLine}`);
      console.log(`  wpa-sec classified the injected upload as: ${channel.uploadResult}`);
      if (channel.uploadResult === 'rejected') {
        console.log('  (rejected is expected for the synthetic stimulus — the serial→inject→drain→TLS');
        console.log('   →POST→parse→resume path on the SHIPPED loop is proven, not a probe)');
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
    `\nPASS — a serial stimulus command drove the shipped hunt loop to inject, enqueue, and drain a ` +
      `capture to wpa-sec, then resume hunting; review ${ARTIFACT_PATH}.`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
