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

/** A line-oriented view over the port, remembering any fatal line seen at any time. */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    port.on('data', (chunk) => this.onData(chunk));
  }

  onData(chunk) {
    this.buffer += chunk.toString('utf8');
    let nl;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl).replace(/\r$/, '');
      this.buffer = this.buffer.slice(nl + 1);
      if (/^\[(FATAL|ERROR)\]/.test(line)) this.fatalLine = line;
      for (const w of this.waiters) w(line);
    }
  }

  send(command) {
    return new Promise((resolve, reject) =>
      this.port.write(`${command}\n`, (err) => (err ? reject(err) : resolve())),
    );
  }

  /** Resolve with the first line matching `pattern`, or reject on timeout. */
  waitForLine(pattern, timeoutMs) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w !== onLine);
        reject(new Error(`timed out waiting for ${pattern}`));
      }, timeoutMs);
      const onLine = (line) => {
        if (!pattern.test(line)) return;
        clearTimeout(timer);
        this.waiters = this.waiters.filter((w) => w !== onLine);
        resolve(line);
      };
      this.waiters.push(onLine);
    });
  }

  /** Collect lines until `end` matches; returns the lines in between. */
  collectUntil(end, timeoutMs) {
    return new Promise((resolve, reject) => {
      const lines = [];
      const timer = setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w !== onLine);
        reject(new Error(`timed out collecting until ${end}`));
      }, timeoutMs);
      const onLine = (line) => {
        if (end.test(line)) {
          clearTimeout(timer);
          this.waiters = this.waiters.filter((w) => w !== onLine);
          resolve(lines);
        } else {
          lines.push(line);
        }
      };
      this.waiters.push(onLine);
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

/** RGB565 little-endian bytes -> 24-bit PNG. */
function rgb565ToPng(width, height, bytes) {
  const raw = Buffer.alloc(height * (1 + width * 3));
  let o = 0;
  for (let y = 0; y < height; y++) {
    raw[o++] = 0; // filter: none
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 2;
      const v = bytes[i] | (bytes[i + 1] << 8);
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
  await new Promise((resolve, reject) =>
    port.on('open', resolve).on('error', reject),
  );
  const channel = new Channel(port);
  console.log(`opened ${path} @ ${BAUD}`);

  try {
    // Scenario D — ping liveness.
    await channel.send('ping');
    await channel.waitForLine(/^\[CMD\] pong/, REPLY_TIMEOUT_MS);

    // Scenario C/D — state, well-formed and carrying geometry.
    await channel.send('state');
    const state1 = await channel.waitForLine(/^\[STATE\]/, REPLY_TIMEOUT_MS);
    const geom = /disp=(\d+)x(\d+)/.exec(state1);
    if (!geom) fail(`[STATE] missing disp=WxH: ${state1}`);
    const heap1 = parseHeapFree(state1);
    if (heap1 === null) fail(`[STATE] missing heap_free: ${state1}`);

    // Scenario D — a second state must report the same free heap (observation is read-only).
    await channel.send('state');
    const state2 = await channel.waitForLine(/^\[STATE\]/, REPLY_TIMEOUT_MS);
    const heap2 = parseHeapFree(state2);
    if (heap1 !== null && heap2 !== null && Math.abs(heap1 - heap2) > HEAP_STABILITY_TOLERANCE) {
      fail(`free heap moved between reads (${heap1} -> ${heap2}); observation is not read-only`);
    }

    // Scenario C — dump the canvas and reconstruct the artifact.
    await channel.send('dump');
    const begin = await channel.waitForLine(/^\[DUMP\] begin/, REPLY_TIMEOUT_MS);
    const dm = /w=(\d+) h=(\d+) bpp=16 bytes=(\d+)/.exec(begin);
    if (!dm) throw new Error(`malformed dump header: ${begin}`);
    const [, w, h, nbytes] = dm.map(Number);
    const hexLines = await channel.collectUntil(/^\[DUMP\] end/, DUMP_TIMEOUT_MS);
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
