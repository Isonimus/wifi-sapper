#!/usr/bin/env node
/**
 * scripts/0032-capture-notify-verify.mjs — device verify for slice-0032 (ADR-0031, lane 3).
 *
 * Drives a flashed Cardputer ADV running the capture probe (SAPPER_TEST_CAPTURE=1) and proves the two
 * device-only halves a host test cannot assert: that an enqueued handshake makes the SerialEventLogger
 * emit `[CAPTURE] essid=…` on real hardware, and that the panel draws the CAPTURED banner (slice-0032
 * Scenario J). Two halves, per docs/quality-bar §3:
 *   - error-check (pass/fail, machine-checkable): a REPEATING `[CAPTURE] essid=…` line (the probe
 *     re-injects on a cadence, so it must recur — an S3 USB-CDC that misses the boot-once line still
 *     sees the next), and no [FATAL]/[ERROR];
 *   - artifact (human-reviewed): the canvas `dump` reconstructed to a PNG so a human can confirm the
 *     CAPTURED banner names the injected network.
 *
 * The announcement fires at enqueue, so this verify needs NO network (unlike verify:upload). Runs on a
 * workstation with the board attached, never in cloud CI (ADR-0004 invariant #5). Port: argv[2], else
 * $SAPPER_PORT, else auto.
 *
 * Usage: node scripts/0032-capture-notify-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PNG = `${ARTIFACT_DIR}/0032-capture-notify.png`;
// Gate on the REPEATING SerialEventLogger line, not a boot-once one: the S3 USB-CDC may not reset on
// port-open, so a one-shot line can be missed (the slice-0028 lesson). The probe re-injects every
// ~1.5 s, so a second line is prompt; allow a couple of cycles plus boot.
const CAPTURE_LINE = /^\[CAPTURE\] essid=/;
const FIRST_TIMEOUT_MS = 20000;
const REPEAT_TIMEOUT_MS = 10000;
const DUMP_TIMEOUT_MS = 30000; // a full 240x135 RGB565 canvas is ~64 KB of hex
// The banner is a large coloured rect over a black HUD, so a correct frame is a mix of colours. A blank
// (all-one-colour) canvas means nothing rendered — require a real minority off the dominant colour.
const MIN_DISTINCT_FRACTION = 0.02;

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
      if (/^\[(FATAL|ERROR)\]/.test(line)) this.fatalLine = line;
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  send(command) {
    return new Promise((resolve, reject) =>
      this.port.write(`${command}\n`, (err) => (err ? reject(err) : resolve())),
    );
  }

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

  /** Capture a bulk dump: ignore lines until `header`, then collect until `end`. */
  captureDump(header, end, timeoutMs) {
    return new Promise((resolve, reject) => {
      if (this.portError) {
        reject(this.portError);
        return;
      }
      let headerLine = null;
      const lines = [];
      const timer = setTimeout(() => {
        this.remove(waiter);
        reject(new Error('timed out capturing dump'));
      }, timeoutMs);
      const waiter = {
        onLine: (line) => {
          if (headerLine === null) {
            if (header.test(line)) headerLine = line;
            return;
          }
          if (end.test(line)) {
            clearTimeout(timer);
            this.remove(waiter);
            resolve({ header: headerLine, lines });
          } else {
            lines.push(line);
          }
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

// --- minimal PNG encoder (no dependencies) — same as slice-0026 -------------

const CRC_TABLE = (() => {
  const table = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c;
  }
  return table;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, 'ascii');
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

function rgb565ToPng(width, height, bytes) {
  const raw = Buffer.alloc(height * (1 + width * 3));
  let o = 0;
  for (let y = 0; y < height; y++) {
    raw[o++] = 0; // filter: none
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 2;
      const v = (bytes[i] << 8) | bytes[i + 1]; // LGFX_Sprite RGB565 is big-endian
      const r5 = (v >> 11) & 0x1f;
      const g6 = (v >> 5) & 0x3f;
      const b5 = v & 0x1f;
      raw[o++] = Math.round((r5 * 255) / 31);
      raw[o++] = Math.round((g6 * 255) / 63);
      raw[o++] = Math.round((b5 * 255) / 31);
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // colour type: truecolour
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  return Buffer.concat([
    signature,
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', deflateSync(raw)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}

/** Fraction of RGB565 pixels that are NOT the single most-common colour. */
function distinctFraction(width, height, bytes) {
  const counts = new Map();
  const n = width * height;
  for (let p = 0; p < n; p++) {
    const v = (bytes[p * 2] << 8) | bytes[p * 2 + 1];
    counts.set(v, (counts.get(v) ?? 0) + 1);
  }
  let top = 0;
  for (const c of counts.values()) if (c > top) top = c;
  return (n - top) / n;
}

async function main() {
  const path = await resolvePort();
  const port = new SerialPort({ path, baudRate: BAUD });
  await new Promise((resolve, reject) => {
    port.once('open', resolve);
    port.once('error', reject);
  });
  const channel = new Channel(port);
  console.log(`opened ${path} @ ${BAUD} — capture-notify verify`);

  try {
    // The gate: the SerialEventLogger's `[CAPTURE] essid=…` line, which fires only when an enqueued
    // handshake was published on the bus and the subscribed logger reacted — on real hardware. Require
    // it to REPEAT so a missed boot-once line does not cause a false pass or fail.
    const first = await channel.waitForLine(CAPTURE_LINE, FIRST_TIMEOUT_MS).catch((e) => {
      fail(
        `no [CAPTURE] essid= line within ${FIRST_TIMEOUT_MS}ms (${e.message}). Check: the board was ` +
          `flashed with SAPPER_TEST_CAPTURE=1 (env:cardputer_testhooks) and reset; watch ` +
          `\`pio device monitor\` for a boot [FATAL].`,
      );
      return null;
    });
    if (first) {
      console.log(`  ${first}`);
      const again = await channel.waitForLine(CAPTURE_LINE, REPEAT_TIMEOUT_MS).catch((e) => {
        fail(`the [CAPTURE] line did not repeat within ${REPEAT_TIMEOUT_MS}ms (${e.message})`);
        return null;
      });
      if (again) console.log(`  ${again} (repeated — the announcement recurs on the injection cadence)`);
    }

    // The eyeball half: reconstruct the panel so a human can confirm the CAPTURED banner.
    const dumpP = channel.captureDump(/^\[DUMP\] begin/, /^\[DUMP\] end/, DUMP_TIMEOUT_MS);
    await channel.send('dump');
    const { header: begin, lines: hexLines } = await dumpP;
    const dm = /w=(\d+) h=(\d+) bpp=16 bytes=(\d+)/.exec(begin);
    if (!dm) throw new Error(`malformed dump header: ${begin}`);
    const [, w, h, nbytes] = dm.map(Number);
    const bytes = Buffer.from(hexLines.join(''), 'hex');
    if (bytes.length !== nbytes) fail(`dump length ${bytes.length} != declared ${nbytes}`);

    if (bytes.length >= w * h * 2) {
      const frac = distinctFraction(w, h, bytes);
      if (frac < MIN_DISTINCT_FRACTION) {
        fail(`canvas is effectively blank (only ${(frac * 100).toFixed(2)}% off the dominant colour) — nothing rendered`);
      }
      mkdirSync(ARTIFACT_DIR, { recursive: true });
      writeFileSync(ARTIFACT_PNG, rgb565ToPng(w, h, bytes));
      console.log(`wrote ${ARTIFACT_PNG} (${w}x${h}); ${(frac * 100).toFixed(1)}% of pixels off the dominant colour`);
    } else {
      fail(`dump too short to form a ${w}x${h} image`);
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
  console.log('\nPASS — a real board announced a capture over serial; review the PNG for the CAPTURED banner.');
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
