#!/usr/bin/env node
/**
 * scripts/0024-webhook-verify.mjs — device verify for slice-0024 (ADR-0004 lane 3, ADR-0023).
 *
 * Proves the runtime claim a unit test cannot: on real hardware a new-password fact really travels the
 * observe→enqueue→window→POST path and reaches a live push endpoint. The notifier *policy* (enqueue on
 * NewPassword, per-target rendering, send-and-keep flush) is lane 1 (test/test_webhook_notifier); this
 * is the on-air half — the surface is really wired into the shipped loop, the supervisor really flushes
 * it inside an STA window, and the Esp32 transport really completes a TLS POST verified against the
 * system CA bundle. The machine-checkable half asserts the transport logged a 2xx send; the human half
 * is confirming the notification actually arrived on the phone / in the Discord channel.
 *
 * The stimulus is the webhook probe: it injects a synthetic new-password fact onto the bus and forces a
 * due sync so the next STA window flushes (POSTs) it, re-driving after each send so a late-joining
 * monitor still witnesses one. Runs on a workstation with the board attached, never in cloud CI (§4
 * invariant #5). Requires `npm install` (serialport).
 *
 * Precondition: flash cardputer_testhooks with real WiFi credentials, a real wpa-sec key, a webhook
 * URL, and SAPPER_TEST_WEBHOOK set —
 *
 *   SAPPER_TEST_WIFI_SSID=... SAPPER_TEST_WIFI_PASS=... SAPPER_TEST_WPASEC_KEY=... \
 *   SAPPER_TEST_WEBHOOK_URL='https://ntfy.sh/your-topic' SAPPER_TEST_WEBHOOK=1 \
 *   pio run -e cardputer_testhooks -t upload
 *
 * The webhook URL is a secret (topic/token) and is never printed by the device or committed. This
 * script observes the running device (it never resets it — this board's native USB-CDC re-enumerates on
 * reset, as verify:provisioning documents).
 *
 * Usage: node scripts/0024-webhook-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PATH = `${ARTIFACT_DIR}/0024-webhook.txt`;
// Association + NTP + one STA window (opened on the drain time ceiling) plus a TLS POST take time; allow
// a generous window to join the running loop and witness a send.
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
 * A line-oriented view over the port. Remembers any fatal line, every [WEBHOOK]/[SYNC]/[HUNT] line (the
 * artifact), and whether the transport reported a 2xx send.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.artifact = [];
    this.sentCode = null;  // the 2xx code from a "[WEBHOOK] sent code=<n>" line, once seen.
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
      if (/^\[(WEBHOOK|SYNC|HUNT)\]/.test(line)) this.artifact.push(line);
      const code = line.match(/^\[WEBHOOK\] sent code=(\d+)/)?.[1];
      if (code && Number(code) >= 200 && Number(code) < 300) this.sentCode = this.sentCode ?? code;
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
  console.log(`opened ${path} @ ${BAUD} — push-webhook verify`);

  try {
    console.log('observing the running webhook loop (the board is not reset — this can take a minute)...');
    // A 2xx send OR a device fatal/error ends the wait. The probe re-drives after each send; a fault or
    // a non-2xx POST prints [FATAL]/[ERROR].
    await channel
      .waitFor(() => channel.sentCode !== null || channel.fatalLine !== null, OBSERVE_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `saw no 2xx webhook send in ${OBSERVE_TIMEOUT_MS}ms (${e.message}); flash ` +
            `cardputer_testhooks with SAPPER_TEST_WEBHOOK=1, a real https webhook URL, and real ` +
            `WiFi/wpa-sec credentials`,
        );
      });

    // A device-reported fatal/error fails the run — the machine-checkable half (§3): the push path must
    // complete cleanly (a failed POST logs [ERROR] webhook POST failed).
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);

    if (failures.length === 0) console.log(`  webhook POST accepted: HTTP ${channel.sentCode}`);
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
    `\nPASS — a new-password fact travelled the bus into an STA window and the transport POSTed it to ` +
      `the live endpoint (HTTP ${channel.sentCode}); confirm the notification actually arrived.`,
  );
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
