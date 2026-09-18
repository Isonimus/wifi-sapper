#!/usr/bin/env node
/**
 * scripts/0005-cardputer-bringup-verify.mjs — device verify for slice-0005 (ADR-0004 lane 3).
 *
 * Drives a flashed Cardputer ADV over the serial control channel's observation half and
 * proves the runtime claims a unit test cannot: the panel came up and the channel answers
 * without mutating state (slice-0005 Scenarios C, D). It has two halves, per docs/quality-bar
 * §3:
 *   - error-check (pass/fail, machine-checkable): fails on any [FATAL]/[ERROR] line, on a
 *     missing or malformed reply, or on the free heap moving between two `state` reads;
 *   - artifact (human-reviewed): reconstructs the canvas `dump` into a PNG so a human can
 *     confirm geometry, colour order, and orientation — the ADR-0002 §5 panel parameters.
 *
 * Runs on a workstation with the board attached, never in cloud CI (ADR-0004 invariant #5).
 * Requires `npm install` (serialport). Port: argv[2], else $SAPPER_PORT, else auto-detect.
 *
 * Usage: node scripts/0005-cardputer-bringup-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT_PNG = `${ARTIFACT_DIR}/0005-cardputer-bringup.png`;
const ARTIFACT_STATE = `${ARTIFACT_DIR}/0005-cardputer-bringup.state.txt`;
const HEAP_STABILITY_TOLERANCE = 1024; // bytes; an allocation-free path should not move heap
const REPLY_TIMEOUT_MS = 4000;
const DUMP_TIMEOUT_MS = 30000; // a full 240x135 RGB565 canvas is ~64 KB of hex

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
 * A line-oriented view over the port, remembering any fatal line seen at any time.
 *
 * Waiters are objects `{ onLine, reject }`. A transport error (e.g. the cable is unplugged
 * mid-run) rejects every pending waiter at once, so a real disconnect fails loud immediately
 * instead of stalling each pending read to its full timeout with a misleading message.
 */
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
      // Copy: a waiter's onLine may remove itself (or, via onError, the list) mid-iteration.
      for (const w of [...this.waiters]) w.onLine(line);
    }
  }

  send(command) {
    return new Promise((resolve, reject) =>
      this.port.write(`${command}\n`, (err) => (err ? reject(err) : resolve())),
    );
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

  /**
   * Capture a bulk dump in a single handler: ignore lines until `header` matches, then
   * collect lines until `end`. One handler with no registration gap — register it *before*
   * sending the command, so the first continuously-streamed lines cannot arrive unlistened.
   */
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

// --- minimal PNG encoder (no dependencies) ---------------------------------

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

/** RGB565 big-endian bytes (LGFX_Sprite order) -> 24-bit PNG. */
function rgb565ToPng(width, height, bytes) {
  const raw = Buffer.alloc(height * (1 + width * 3));
  let o = 0;
  for (let y = 0; y < height; y++) {
    raw[o++] = 0; // filter: none
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 2;
      // LGFX_Sprite stores RGB565 MSB-first (big-endian) — that is the order it DMAs to the
      // SPI panel, and getBuffer() hands back that raw buffer. Decode big-endian to match.
      const v = (bytes[i] << 8) | bytes[i + 1];
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

function parseHeapFree(stateLine) {
  const m = /heap_free=(\d+)/.exec(stateLine);
  return m ? Number(m[1]) : null;
}

async function main() {
  const path = await resolvePort();
  const port = new SerialPort({ path, baudRate: BAUD });
  // `.once` for the open phase: after open resolves, Channel installs the persistent 'error'
  // handler. A lingering `.on('error', reject)` would be a stale no-op that swallows later
  // transport errors.
  await new Promise((resolve, reject) => {
    port.once('open', resolve);
    port.once('error', reject);
  });
  const channel = new Channel(port);
  console.log(`opened ${path} @ ${BAUD}`);

  try {
    // Every request registers its listener BEFORE sending, so a reply (or the first line of a
    // bulk dump) that arrives immediately cannot land in a gap with no handler.

    // Scenario D — ping liveness.
    const pong = channel.waitForLine(/^\[CMD\] pong/, REPLY_TIMEOUT_MS);
    await channel.send('ping');
    await pong;

    // Scenario C/D — state, well-formed and carrying geometry.
    const state1p = channel.waitForLine(/^\[STATE\]/, REPLY_TIMEOUT_MS);
    await channel.send('state');
    const state1 = await state1p;
    const geom = /disp=(\d+)x(\d+)/.exec(state1);
    if (!geom) fail(`[STATE] missing disp=WxH: ${state1}`);
    const heap1 = parseHeapFree(state1);
    if (heap1 === null) fail(`[STATE] missing heap_free: ${state1}`);

    // Scenario D — a second state must report the same free heap (observation is read-only).
    const state2p = channel.waitForLine(/^\[STATE\]/, REPLY_TIMEOUT_MS);
    await channel.send('state');
    const state2 = await state2p;
    const heap2 = parseHeapFree(state2);
    if (heap2 === null) fail(`[STATE] missing heap_free on 2nd read: ${state2}`);
    if (heap1 !== null && heap2 !== null && Math.abs(heap1 - heap2) > HEAP_STABILITY_TOLERANCE) {
      fail(`free heap moved between reads (${heap1} -> ${heap2}); observation is not read-only`);
    }

    // Scenario C — dump the canvas and reconstruct the artifact.
    const dumpP = channel.captureDump(/^\[DUMP\] begin/, /^\[DUMP\] end/, DUMP_TIMEOUT_MS);
    await channel.send('dump');
    const { header: begin, lines: hexLines } = await dumpP;
    const dm = /w=(\d+) h=(\d+) bpp=16 bytes=(\d+)/.exec(begin);
    if (!dm) throw new Error(`malformed dump header: ${begin}`);
    const [, w, h, nbytes] = dm.map(Number);
    const hex = hexLines.join('');
    const bytes = Buffer.from(hex, 'hex');
    if (bytes.length !== nbytes) {
      fail(`dump length ${bytes.length} != declared ${nbytes}`);
    }
    if (bytes.length >= w * h * 2) {
      mkdirSync(ARTIFACT_DIR, { recursive: true });
      writeFileSync(ARTIFACT_PNG, rgb565ToPng(w, h, bytes));
      writeFileSync(ARTIFACT_STATE, `${state1}\n${state2}\n`);
      console.log(`wrote ${ARTIFACT_PNG} (${w}x${h}) and ${ARTIFACT_STATE}`);
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
  console.log('\nPASS — bring-up verified; review the PNG artifact for panel correctness.');
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
