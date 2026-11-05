#!/usr/bin/env node
/**
 * scripts/pwncrack-response-probe.mjs — pin the pwncrack.org upload-response JSON schema.
 *
 * Purpose: capture pwncrack's `POST /upload_handshake` responses across the cases the classifier must
 * tell apart, so the pwncrack upload classifier (ADR-0109 decision #6) is pinned by measured data, not
 * a guess. The first run (2026-11-05) showed a fresh upload and its immediate re-upload BOTH return
 * HTTP 200 `{"success": true}` — so pwncrack has no duplicate signal (fine: UploadResult::Accepted and
 * Duplicate are already treated identically). The open question that remains is the NEGATIVE shape: if a
 * wrong key or a malformed hashline ALSO returns `{"success": true}`, then that token means only
 * "request received", not "accepted", and a classifier that trusts it would fail OPEN (mark a capture
 * submitted that pwncrack never queued, silently — the exact fail-open ADR-0076 forbids for wpa-sec).
 * So this probe sends four legs — accept, re-upload, malformed content, bad key — and reports whether
 * the positive token leaks into a negative leg. A probe, not a verify: runs once, result lands in the
 * ADR/fixtures, not wired into package.json (§3 R11 exempt).
 *
 * It runs on the workstation (public CA trust is fine — not the device TLS path), uses HTTPS (never the
 * plugin's http: the key must not cross the wire in clear), and is OUTWARD-FACING: it uploads a real
 * handshake to pwncrack.org (legs 1-3) plus junk payloads (legs 3-4). Run it only with a handshake you
 * are authorized to submit.
 *
 * Privacy (§4 #25): raw bodies may echo the uploaded BSSID/ESSID, so they go ONLY to the gitignored
 * src/gen/ sink for your eyes; the COMMITTABLE artifact gets a redacted structural summary (field names
 * and value *types*, never values). The API key is read from the environment, never printed or written.
 *
 * Precondition: SAPPER_TEST_PWNCRACK_KEY (or PWNCRACK_KEY) is a valid pwncrack account key, and a real
 * WPA*01 `.hc22000` line file is given (argv[1] or SAPPER_TEST_HC22000).
 *
 * Usage:
 *   SAPPER_TEST_PWNCRACK_KEY=<key> node scripts/pwncrack-response-probe.mjs path/to/capture.hc22000
 */
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs';

const UPLOAD_URL = 'https://pwncrack.org/upload_handshake';
const RAW_SINK = 'src/gen/pwncrack-response-sample.txt'; // gitignored: real identifiers for your eyes.
const ARTIFACT = 'artifacts/pwncrack-response-probe.txt'; // committable: redacted schema only.

// A key that cannot be anyone's real key, for the bad-key leg. Not a secret; a fixed junk string.
const BOGUS_KEY = 'probe-invalid-key-000000000000';
// A syntactically-prefixed but content-invalid hashline, for the malformed-content leg.
const MALFORMED_HC22000 = Buffer.from('WPA*01*0000000000000000\n', 'latin1');

// Response headers worth recording: content-type decides JSON-vs-HTML (a Cloudflare challenge is HTML),
// and the cf-* / server headers confirm whether Cloudflare fronted or challenged the request (the LEDGER
// first-contact-behind-Cloudflare gate). Deliberately NOT the full header set (it can carry a token).
const HEADERS_OF_INTEREST = ['content-type', 'server', 'x-powered-by', 'cf-ray', 'cf-mitigated', 'retry-after'];

function fatal(message) {
  console.error(`FATAL: ${message}`);
  process.exit(1);
}

function getKey() {
  const key = process.env.SAPPER_TEST_PWNCRACK_KEY || process.env.PWNCRACK_KEY;
  if (!key) {
    fatal(
      'SAPPER_TEST_PWNCRACK_KEY or PWNCRACK_KEY not set.\n' +
        '  Usage: SAPPER_TEST_PWNCRACK_KEY=<key> node scripts/pwncrack-response-probe.mjs <capture.hc22000>',
    );
  }
  return key;
}

function getHandshake() {
  const path = process.argv[2] || process.env.SAPPER_TEST_HC22000;
  if (!path) fatal('No .hc22000 path given (argv[1] or SAPPER_TEST_HC22000).');
  let bytes;
  try {
    bytes = readFileSync(path);
  } catch (err) {
    fatal(`cannot read handshake file "${path}": ${err.message}`);
  }
  // A WPA*01/WPA*02 line is the only thing pwncrack takes; refuse anything else loudly rather than probe
  // with a payload that can only ever return an error body (quality bar §3, fail loud).
  const head = bytes.subarray(0, 7).toString('latin1');
  if (head !== 'WPA*01*' && head !== 'WPA*02*') {
    fatal(`"${path}" does not start with WPA*01*/WPA*02* (got "${head}") — not a hashcat 22000 line.`);
  }
  return { path, bytes };
}

/** One POST of @p bytes authenticated by @p key. Returns status, headers of interest, and the raw body. */
async function upload(key, bytes) {
  if (typeof fetch === 'undefined' || typeof FormData === 'undefined' || typeof Blob === 'undefined') {
    fatal('this probe needs the global fetch/FormData/Blob (Node >= 18; you have ' + process.version + ').');
  }
  const form = new FormData();
  form.append('key', key); // multipart field, exactly as the verified API expects (ADR-0109 context).
  form.append('handshake', new Blob([bytes], { type: 'application/octet-stream' }), 'capture.hc22000');

  let res;
  try {
    res = await fetch(UPLOAD_URL, { method: 'POST', body: form });
  } catch (err) {
    // A transport/TLS failure is itself the probe's answer for that leg; record it rather than crash.
    return { status: 0, statusText: `transport error: ${err.message}`, headers: {}, body: '' };
  }
  const headers = {};
  for (const name of HEADERS_OF_INTEREST) {
    const value = res.headers.get(name);
    if (value !== null) headers[name] = value;
  }
  const body = await res.text();
  return { status: res.status, statusText: res.statusText, headers, body };
}

/** True if the response looks like a Cloudflare interstitial rather than the pwncrack app (LEDGER gate). */
function looksLikeCloudflareChallenge({ status, headers, body }) {
  const ct = (headers['content-type'] || '').toLowerCase();
  if (ct.includes('text/html')) return true;
  const markers = ['just a moment', 'cf-chl', 'challenge-platform', 'attention required', '__cf'];
  const low = body.toLowerCase();
  return markers.some((m) => low.includes(m)) && status !== 200;
}

/** Recursively replace every scalar with its type name, keeping keys/structure — the schema, no values. */
function redactShape(value) {
  if (value === null) return '<null>';
  if (Array.isArray(value)) {
    return value.length === 0 ? '[]' : [redactShape(value[0]), `...x${value.length}`];
  }
  if (typeof value === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(value)) out[k] = redactShape(v);
    return out;
  }
  return `<${typeof value}>`;
}

function tryParse(body) {
  try {
    return { ok: true, value: JSON.parse(body) };
  } catch {
    return { ok: false, value: null };
  }
}

/** The positive token the first run observed: HTTP 200 with a JSON body whose `success` is exactly true. */
function isPositiveToken(leg, parsed) {
  return leg.status === 200 && parsed.ok && parsed.value && parsed.value.success === true;
}

function summariseLeg(label, expect, leg) {
  const parsed = tryParse(leg.body);
  return {
    label,
    expect,
    status: leg.status,
    statusText: leg.statusText,
    headers: leg.headers,
    cloudflareChallenge: looksLikeCloudflareChallenge(leg),
    isJson: parsed.ok,
    positive: isPositiveToken(leg, parsed),
    shape: parsed.ok ? redactShape(parsed.value) : '<non-JSON body>',
    parsed: parsed.value,
    body: leg.body,
    bodyLen: leg.body.length,
  };
}

async function main() {
  const key = getKey();
  const { path, bytes } = getHandshake();
  console.log(`probing ${UPLOAD_URL} with ${path} (${bytes.length} bytes), four legs...\n`);

  // The four cases the classifier must tell apart. Legs 1-2 confirm the positive (and no duplicate
  // signal); legs 3-4 are the negatives that decide whether the positive token can be trusted.
  const plan = [
    { label: '1 accept', expect: 'HTTP 200 {"success":true}', key, bytes },
    { label: '2 re-upload (duplicate?)', expect: 'same as #1 (pwncrack has no dup signal)', key, bytes },
    { label: '3 malformed content', expect: 'a NON-success reject (if it also says success:true, the token is untrustworthy)', key, bytes: MALFORMED_HC22000 },
    { label: '4 bad key', expect: 'an auth reject (if it also says success:true, the token is untrustworthy)', key: BOGUS_KEY, bytes },
  ];

  const legs = [];
  for (const step of plan) {
    legs.push(summariseLeg(step.label, step.expect, await upload(step.key, step.bytes)));
  }

  const negatives = legs.filter((l) => l.label.startsWith('3') || l.label.startsWith('4'));
  const tokenLeaks = negatives.filter((l) => l.positive);

  // --- stdout: full detail for YOUR eyes (the operator's terminal, not a committed file). ---
  for (const leg of legs) {
    console.log(`=== leg ${leg.label} (expect: ${leg.expect}) ===`);
    console.log(`HTTP ${leg.status} ${leg.statusText}  (${leg.bodyLen} bytes${leg.isJson ? ', JSON' : ', NON-JSON'})  positive-token=${leg.positive}`);
    console.log('headers:', JSON.stringify(leg.headers));
    if (leg.cloudflareChallenge) console.log('  ** WARNING: looks like a Cloudflare challenge, not the pwncrack app **');
    console.log(leg.isJson ? JSON.stringify(leg.parsed) : leg.body.slice(0, 2000));
    console.log('');
  }
  console.log('=== verdict ===');
  if (tokenLeaks.length === 0) {
    console.log('GOOD: the positive token (HTTP 200 + {"success":true}) did NOT appear on any negative leg,');
    console.log('      so it is a trustworthy "accepted" signal. Classifier: Accepted on it, Rejected on all else.');
  } else {
    console.log('CAUTION: the positive token ALSO appeared on: ' + tokenLeaks.map((l) => l.label).join(', ') + '.');
    console.log('         So {"success":true} means only "request received", not "accepted/authenticated".');
    console.log('         Report this back: the classifier design must change (see ADR-0109 #6).');
  }

  // --- gitignored raw sink: real bodies for your eyes (may echo BSSID/ESSID). ---
  mkdirSync('src/gen', { recursive: true });
  const raw =
    `pwncrack upload-response raw sample (gitignored; real identifiers)\n` +
    `generated: ${new Date().toISOString()}\n\n` +
    legs
      .map((l) => `=== leg ${l.label} ===\nHTTP ${l.status} ${l.statusText}\nheaders: ${JSON.stringify(l.headers)}\n${l.body}\n`)
      .join('\n');
  writeFileSync(RAW_SINK, raw);

  // --- committable artifact: redacted schema only (field names + value TYPES, never values). ---
  mkdirSync('artifacts', { recursive: true });
  const redacted =
    `pwncrack upload-response schema probe (ADR-0109) — redacted: field names + value types only\n` +
    `generated: ${new Date().toISOString()}\n` +
    `source: POST ${UPLOAD_URL}, four legs (accept / re-upload / malformed content / bad key)\n\n` +
    legs
      .map(
        (l) =>
          `=== leg ${l.label} (expect: ${l.expect}) ===\n` +
          `HTTP status: ${l.status}\n` +
          `content-type: ${l.headers['content-type'] || '(none)'}\n` +
          `cloudflare-challenge: ${l.cloudflareChallenge}\n` +
          `is-json: ${l.isJson}\n` +
          `positive-token (HTTP 200 + success===true): ${l.positive}\n` +
          `shape:\n${JSON.stringify(l.shape, null, 2)}\n`,
      )
      .join('\n') +
    `\n=== verdict ===\n` +
    (tokenLeaks.length === 0
      ? `positive token is trustworthy (absent from every negative leg): Accepted on HTTP 200 + {"success":true}, Rejected on all else.\n`
      : `positive token LEAKED into negative leg(s): ${tokenLeaks.map((l) => l.label).join(', ')}. Classifier design must change (ADR-0109 #6).\n`) +
    `\nNext: paste the stdout back so the PwncrackResponseClassifier fixtures are frozen on measured data (quality bar §3).\n`;
  writeFileSync(ARTIFACT, redacted);

  console.log(`\nwrote raw (gitignored) -> ${RAW_SINK}`);
  console.log(`wrote redacted schema  -> ${ARTIFACT}`);
  if (tokenLeaks.length > 0) process.exit(2); // non-zero: the positive token is not trustworthy as-is.
}

main().catch((err) => fatal(err.stack || String(err)));
