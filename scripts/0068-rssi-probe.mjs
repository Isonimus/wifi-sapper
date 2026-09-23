#!/usr/bin/env node
/**
 * scripts/0068-rssi-probe.mjs — RSSI↔capture-correlation probe reader (ADR-0067, lane 3).
 *
 * A probe, not a wired regression verify (docs/quality-bar §3: a probe answers a measurable question once
 * and the number lands in the ADR). It answers the necessary condition ADR-0067 defers to hardware: does a
 * stronger beacon RSSI predict a higher capture rate? If not, RSSI-ordering cannot raise yield and the §4
 * #8 seam is not widened; if it does, that is sufficient evidence to widen it.
 *
 * The multi-hour run is UNTETHERED: the firmware accumulates a per-band histogram to flash and dumps it to
 * serial both at boot and periodically (this board's native USB re-enumerates on reset, so a boot-only dump
 * is unreliable to catch). So the normal flow is — connect this reader to the running board and wait; it
 * catches the next `[RSSI] histogram …` block the firmware prints, writes the correlation table to an
 * artifact, then tails live `[RSSI] rssi=…` rows (useful for a tethered smoke test) until Ctrl-C or --minutes.
 *
 * The dump carries no BSSID/SSID (§4 #25 holds by construction). Interactive and hardware-attached — never
 * cloud CI (ADR-0004 invariant #5). Needs no network. Deauth only your own authorized networks.
 *
 * Usage: node scripts/0068-rssi-probe.mjs [/dev/ttyACM0] [--minutes 30] [--out artifacts/0068-rssi-correlation.txt]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';

const BAUD = 115200;
const DUMP_TIMEOUT_MS = 30000; // must catch a periodic [RSSI] histogram block within this (cadence is ~15s).
const BAND_LABELS = {
  0: '>= -50 (very strong)',
  1: '-50..-60 (strong)',
  2: '-60..-70 (moderate)',
  3: '-70..-80 (weak)',
  4: '< -80 (very weak)',
};

function parseArgs() {
  const args = process.argv.slice(2);
  const opts = { port: undefined, minutes: undefined, out: 'artifacts/0068-rssi-correlation.txt' };
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--minutes') opts.minutes = Number(args[++i]);
    else if (args[i] === '--out') opts.out = args[++i];
    else if (!args[i].startsWith('--')) opts.port = args[i];
  }
  return opts;
}

async function resolvePort(explicit) {
  const chosen = explicit || process.env.SAPPER_PORT;
  if (chosen) return chosen;
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
    this.lineHandlers = [];
    this.fatalLine = null;
    port.on('data', (chunk) => this.onData(chunk));
  }

  onData(chunk) {
    this.buffer += chunk.toString('utf8');
    let nl;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl).replace(/\r$/, '');
      this.buffer = this.buffer.slice(nl + 1);
      if (/^\[FATAL\]/.test(line)) this.fatalLine = line;
      for (const h of this.lineHandlers) h(line);
    }
  }

  onLine(handler) {
    this.lineHandlers.push(handler);
  }
}

const BAND_ROW = /^\[RSSI\] band=(\d+) label=\S+ attempts=(\d+) captured=(\d+)/;
const NA_ROW = /^\[RSSI\] band=na attempts=(\d+) captured=(\d+)/;
const HEAD_ROW = /^\[RSSI\] histogram tag=(\S+) total_attempts=(\d+) total_captured=(\d+)/;

/**
 * Collect the [RSSI] histogram … end block into a structured table. Installs ONE persistent line handler
 * synchronously before any await, so a dump whose lines the OS coalesces into a single serial chunk is
 * fully captured — registering the handler after matching the head line would drop every following line in
 * that same chunk (they are processed before the awaited continuation runs).
 */
function readHistogram(channel) {
  return new Promise((resolve, reject) => {
    const table = { tag: null, totalAttempts: 0, totalCaptured: 0, bands: {}, na: null };
    const timer = setTimeout(
      () => reject(new Error(`no complete [RSSI] histogram block within ${DUMP_TIMEOUT_MS}ms`)),
      DUMP_TIMEOUT_MS,
    );
    channel.onLine((line) => {
      const h = HEAD_ROW.exec(line);
      if (h) {
        table.tag = h[1];
        table.totalAttempts = Number(h[2]);
        table.totalCaptured = Number(h[3]);
        return;
      }
      const b = BAND_ROW.exec(line);
      if (b) {
        table.bands[Number(b[1])] = { attempts: Number(b[2]), captured: Number(b[3]) };
        return;
      }
      const n = NA_ROW.exec(line);
      if (n) {
        table.na = { attempts: Number(n[1]), captured: Number(n[2]) };
        return;
      }
      if (/^\[RSSI\] histogram end/.test(line)) {
        // With periodic dumps, attaching to a running board can join a block mid-stream (after its head
        // line, before its end). That partial block has no head, so ignore this end and keep waiting for
        // the next complete head→…→end — a stray end is normal here, not an error.
        if (table.tag === null) return;
        clearTimeout(timer);
        resolve(table);
      }
    });
  });
}

function formatTable(table) {
  const rate = (c, a) => (a > 0 ? `${((100 * c) / a).toFixed(1)}%` : '   -');
  const lines = [];
  lines.push('RSSI-to-capture correlation (ADR-0067) — identity-free, no BSSID recorded');
  lines.push(`generated: ${new Date().toISOString()}`);
  lines.push(`run tag:   ${table.tag}`);
  lines.push('');
  lines.push(`attempts total: ${table.totalAttempts}    captured total: ${table.totalCaptured}`);
  lines.push('');
  lines.push('band                    captured / total   rate');
  lines.push('----                    ----------------   ----');
  for (let i = 0; i < 5; i++) {
    const b = table.bands[i] ?? { attempts: 0, captured: 0 };
    lines.push(
      `${BAND_LABELS[i].padEnd(23)} ${String(b.captured).padStart(6)} / ${String(b.attempts).padEnd(6)}   ${rate(b.captured, b.attempts)}`,
    );
  }
  const na = table.na ?? { attempts: 0, captured: 0 };
  lines.push(`${'rssi=na (unknown)'.padEnd(23)} ${String(na.captured).padStart(6)} / ${String(na.attempts).padEnd(6)}   ${rate(na.captured, na.attempts)}`);
  lines.push('');
  lines.push('Read: a monotonically higher rate toward stronger bands is the signal that RSSI-ordering');
  lines.push('would raise yield (justifies widening §4 #8). A flat rate across bands is the null result');
  lines.push('that kills RSSI-ordering. Put the table in the follow-up seam-decision ADR.');
  return lines.join('\n');
}

async function main() {
  const opts = parseArgs();
  const path = await resolvePort(opts.port);
  const port = new SerialPort({ path, baudRate: BAUD });
  await new Promise((resolve, reject) => {
    port.once('open', resolve);
    port.once('error', reject);
  });
  const channel = new Channel(port);
  console.log(`opened ${path} @ ${BAUD}`);
  console.log('>>> waiting for the next periodic [RSSI] histogram block (no reset needed)...\n');

  let table;
  try {
    table = await readHistogram(channel);
  } catch (err) {
    port.close();
    throw new Error(`${err.message} — did you reset the board after connecting?`);
  }

  const report = formatTable(table);
  console.log(`\n${report}\n`);
  mkdirSync(dirname(opts.out), { recursive: true });
  writeFileSync(opts.out, `${report}\n`);
  console.log(`written: ${opts.out}`);

  if (channel.fatalLine) {
    port.close();
    console.error(`\ndevice reported: ${channel.fatalLine}`);
    process.exit(1);
  }

  // Tail live rows for a tethered smoke session (the artifact above is already saved).
  console.log(`\ntailing live attempts (${opts.minutes ? `for ${opts.minutes} min` : 'Ctrl-C to stop'})...\n`);
  channel.onLine((line) => {
    if (/^\[RSSI\] (rssi=|waiting )/.test(line)) console.log(line);
  });
  await new Promise((resolve) => {
    let done = false;
    let timer = null;
    const finish = () => {
      if (done) return;
      done = true;
      if (timer) clearTimeout(timer); // release the --minutes timer so Ctrl-C exits immediately.
      process.removeListener('SIGINT', finish);
      resolve();
    };
    process.once('SIGINT', finish);
    if (opts.minutes) timer = setTimeout(finish, opts.minutes * 60_000);
  });
  port.close();
}

main().catch((err) => {
  console.error(`\nERROR: ${err.message}`);
  process.exit(1);
});
