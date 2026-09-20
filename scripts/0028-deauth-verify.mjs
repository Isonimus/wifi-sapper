#!/usr/bin/env node
/**
 * scripts/0028-deauth-verify.mjs — device verify for slice-0028 (ADR-0004 lane 3, ADR-0027).
 *
 * Proves the runtime claims a unit test cannot: that the ESP32 actually puts real deauth/disassoc
 * frames on the air. The frame byte layout is lane 1 (test/test_deauth); this is the on-air half.
 * Runs on a workstation with the board attached, never in cloud CI (§4 invariant #5). Requires
 * `npm install` (serialport).
 *
 * TWO proofs, matching the slice's Definition of Done:
 *   J (deterministic gate, always runs — the pass/fail): the SDK sanity-check bypass is active and
 *     the device transmits deauth+disassoc frames at the target with transmit() succeeding (txOk>0),
 *     no [FATAL]/[ERROR]. Needs no other station.
 *   K (opportunistic e2e, not the gate): if — during the run — a client on the target AP reconnects
 *     and its forced handshake is captured, the probe streams a wpa-sec-valid pcap, saved as the
 *     owed evidence. Absent, the gate still passes.
 *
 * *** AUTHORIZED USE ONLY. *** Deauth is a transmit attack: point SAPPER_TEST_DEAUTH_BSSID only at a
 * network you own or are explicitly authorized to test. This script does not enforce that — you do.
 *
 * Precondition: flash the `cardputer_testhooks` build with the target set —
 *
 *   SAPPER_TEST_DEAUTH_BSSID=aa:bb:cc:dd:ee:ff SAPPER_TEST_DEAUTH_CHANNEL=6 \
 *     [SAPPER_TEST_DEAUTH_CLIENT=11:22:33:44:55:66] \
 *     pio run -e cardputer_testhooks -t upload
 *
 * Usage: node scripts/0028-deauth-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const LOG_PATH = `${ARTIFACT_DIR}/0028-deauth-tx.txt`;
const PCAP_PATH = `${ARTIFACT_DIR}/0028-deauth-forced-handshake.pcap`;
// Gate on the REPEATING txOk heartbeat (printed every 2s), not the boot-once target line — the
// board may have booted before this script opened the port, so a one-shot line can be missed.
const HEARTBEAT_TIMEOUT_MS = 10000; // first heartbeat lands ~2s after the probe starts
const CAPTURE_WAIT_MS = 15000; // opportunistic window for a forced handshake to arrive (not the gate)

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
 * A line-oriented view over the port. Remembers any fatal line, every [DEAUTH] line (the log
 * artifact), and reconstructs a hex-framed pcap between the BEGIN/END markers if one arrives.
 */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.deauthLines = [];
    this.portError = null;
    this.pcapHex = null; // non-null once BEGIN seen; the accumulating hex
    this.pcapBytes = null; // Buffer once END seen
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
    // Reconstruct the opportunistic pcap: BEGIN, one hex line, END.
    if (/^\[DEAUTH-PCAP-BEGIN\]/.test(line)) {
      this.pcapHex = '';
      return;
    }
    if (/^\[DEAUTH-PCAP-END\]/.test(line)) {
      if (this.pcapHex && /^[0-9A-Fa-f]+$/.test(this.pcapHex)) {
        this.pcapBytes = Buffer.from(this.pcapHex, 'hex');
      }
      this.pcapHex = null;
      return;
    }
    if (this.pcapHex !== null) this.pcapHex += line.trim();
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
  console.log(`opened ${path} @ ${BAUD} — deauth verify`);

  try {
    // The gate: a repeating heartbeat reporting at least one successful transmit. txOk>0 proves the
    // probe is active, the SDK bypass is working (a blocked frame would land in txFail, and a failed
    // bypass aborts the probe before any heartbeat), and real frames are going on the air.
    const tx = await channel
      .waitForLine(/^\[DEAUTH\] txOk=(\d+)/, HEARTBEAT_TIMEOUT_MS)
      .catch((e) => {
        fail(
          `no [DEAUTH] txOk heartbeat within ${HEARTBEAT_TIMEOUT_MS}ms (${e.message}). Check: ` +
            `SAPPER_TEST_DEAUTH_BSSID is set for the flashed build; no OTHER SAPPER_TEST_* var is ` +
            `set (an earlier probe — e.g. SAPPER_TEST_SCREEN — takes over first); and watch ` +
            `\`pio device monitor\` right after reset for a boot-time [FATAL] (e.g. promiscuous ` +
            `begin failed). An ineffective bypass instead shows up as heartbeats with txOk=0`,
        );
        return null;
      });

    if (tx) {
      console.log(`  ${tx}`);
      const txOk = Number(tx.match(/txOk=(\d+)/)[1]);
      if (txOk === 0) fail('device reported txOk=0 — the radio rejected every deauth frame');

      // Opportunistic e2e (not the gate): give a forced handshake a window to arrive.
      if (!channel.pcapBytes) {
        console.log(`  waiting up to ${CAPTURE_WAIT_MS}ms for an opportunistic forced handshake...`);
        await channel
          .waitForLine(/^\[DEAUTH-PCAP-END\]/, CAPTURE_WAIT_MS)
          .catch(() => null); // absence is fine — e2e is environmental, not pass/fail
      }
    }

    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    writeFileSync(LOG_PATH, `${channel.deauthLines.join('\n')}\n`);
    console.log(`wrote ${LOG_PATH}`);
    if (channel.pcapBytes) {
      writeFileSync(PCAP_PATH, channel.pcapBytes);
      console.log(`wrote ${PCAP_PATH} (${channel.pcapBytes.length} bytes) — forced handshake captured`);
    }
    port.close();
  }

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  const e2e = channel.pcapBytes
    ? `and forced a handshake into ${PCAP_PATH}`
    : '(no forced handshake this run — opportunistic, not required)';
  console.log(`\nPASS — the device transmitted real deauth/disassoc frames on air ${e2e}; review ${LOG_PATH}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
