#!/usr/bin/env node
/**
 * scripts/wpasec-download-probe.mjs — measure the wpa-sec `?api&dl=1` response format.
 *
 * Purpose: capture ONE live download response so the line format (colons, field widths, and
 * validation rules) are pinned by measured data. This is the probe the measurement lands in an
 * ADR; it runs on the workstation (public CA trust is fine here — this is not the device path),
 * issues one GET against the live wpa-sec service authenticated by the operator's key, and writes
 * the raw body to src/gen/wpasec-cracked-sample.txt (a gitignored directory for sensitive data).
 *
 * The output is a structural analysis: line counts, colon distribution, and a count of lines the
 * device parser would reject (fewer than 3 colons, AP-BSSID not exactly 12 hex nibbles, or empty
 * essid/password). This data goes into the ADR so future parser changes are measured against the
 * real account format, not a guess (ADR-0019 decision #3, quality bar §4).
 *
 * Precondition: SAPPER_TEST_WPASEC_KEY or WPASEC_KEY is set to a valid wpa-sec account key.
 * Usage: SAPPER_TEST_WPASEC_KEY=... node scripts/wpasec-download-probe.mjs
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { get } from 'node:https';
import { URL } from 'node:url';

const GEN_DIR = 'src/gen';
const SAMPLE_PATH = `${GEN_DIR}/wpasec-cracked-sample.txt`;

/** Fetch the key from environment. */
function getKey() {
  const key = process.env.SAPPER_TEST_WPASEC_KEY || process.env.WPASEC_KEY;
  if (!key) {
    console.error(
      'FATAL: SAPPER_TEST_WPASEC_KEY or WPASEC_KEY not set.\n' +
        'Usage: SAPPER_TEST_WPASEC_KEY=<key> node scripts/wpasec-download-probe.mjs',
    );
    process.exit(1);
  }
  return key;
}

// Field caps must track src/net/cracked_result.h (kCrackedEssidCap / kCrackedPasswordCap): the device
// parser rejects a field whose length is >= the cap (it would not fit with its NUL terminator).
const ESSID_CAP = 33;
const PASSWORD_CAP = 65;

/**
 * Mirror the device parser (src/net/cracked_result_parser.cpp) exactly, so the reported count matches
 * what the firmware would skip. The line is `ap_bssid:client_bssid:essid:password`, split on the FIRST
 * THREE colons only (the password is the verbatim remainder and may itself contain colons). The client
 * BSSID (field 2) is ignored. A line is rejected when it has fewer than three colons, an AP BSSID that
 * is not exactly 12 bare hex nibbles (no separators), or an empty / over-long essid or password.
 */
function isRejectedLine(line) {
  const trimmed = line.replace(/[\r\n]+$/, '');
  if (trimmed.length === 0) return true;

  const colon1 = trimmed.indexOf(':');
  if (colon1 < 0) return true;
  const colon2 = trimmed.indexOf(':', colon1 + 1);
  if (colon2 < 0) return true;
  const colon3 = trimmed.indexOf(':', colon2 + 1);
  if (colon3 < 0) return true;

  const apBssid = trimmed.slice(0, colon1);
  const essid = trimmed.slice(colon2 + 1, colon3);
  const password = trimmed.slice(colon3 + 1);

  if (!/^[0-9a-fA-F]{12}$/.test(apBssid)) return true;  // exactly 12 hex nibbles, no separators.
  if (essid.length === 0 || essid.length >= ESSID_CAP) return true;
  if (password.length === 0 || password.length >= PASSWORD_CAP) return true;

  return false;
}

/**
 * Perform the HTTPS GET and collect the body. Returns the raw body string on success,
 * throws on failure.
 */
function fetchDownload(key) {
  return new Promise((resolve, reject) => {
    const url = new URL('https://wpa-sec.stanev.org/?api&dl=1');
    const options = {
      hostname: url.hostname,
      path: url.pathname + url.search,
      method: 'GET',
      headers: {
        'Cookie': `key=${key}`,
      },
      timeout: 15000,
    };

    let body = '';
    const req = get(options, (res) => {
      if (res.statusCode !== 200) {
        reject(new Error(`HTTP ${res.statusCode}`));
        return;
      }
      res.on('data', (chunk) => {
        body += chunk.toString('utf8');
      });
      res.on('end', () => {
        resolve(body);
      });
    });

    req.on('error', reject);
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('timeout'));
    });
  });
}

async function main() {
  const key = getKey();
  console.log('fetching live wpa-sec account (one GET against the live service)...');

  let body;
  try {
    body = await fetchDownload(key);
  } catch (err) {
    console.error(`FATAL: ${err.message}`);
    process.exit(1);
  }

  // Write the body to src/gen/wpasec-cracked-sample.txt.
  mkdirSync(dirname(SAMPLE_PATH), { recursive: true });
  writeFileSync(SAMPLE_PATH, body);
  console.log(
    `wrote ${SAMPLE_PATH} (this directory is gitignored for privacy — contains real ESSIDs and passwords)`,
  );

  // Analyze the structure, mirroring the device parser exactly. The parser strips a trailing CR/LF and
  // treats a then-empty line as Empty (ignored, NOT counted as malformed); every other line — including a
  // whitespace-only line, which has no colons — is Ok or Malformed. So a plain trim() pre-filter would
  // wrongly hide whitespace-only lines the firmware counts; strip only the trailing CR and keep the rest.
  const rawLines = body.split('\n');
  const totalLines = rawLines.length;
  const contentLines = rawLines.map((line) => line.replace(/\r$/, '')).filter((line) => line.length > 0);

  // Count colons per content line.
  const colonCounts = {};
  for (const line of contentLines) {
    const count = (line.match(/:/g) || []).length;
    colonCounts[count] = (colonCounts[count] || 0) + 1;
  }

  // Count rejected lines.
  const rejectedCount = contentLines.filter(isRejectedLine).length;
  const acceptedCount = contentLines.length - rejectedCount;

  // Report the measured data.
  console.log('\nMeasured format (data for ADR-0019 decision #3):');
  console.log(`  total line count: ${totalLines}`);
  console.log(`  content lines (non-empty after CR strip): ${contentLines.length}`);
  console.log(`  accepted by parser: ${acceptedCount}`);
  console.log(`  rejected by parser: ${rejectedCount}`);
  console.log(`  colon distribution: ${JSON.stringify(colonCounts)}`);

  if (rejectedCount > 0) {
    console.log(`\nNote: ${rejectedCount} line(s) would be rejected by the device parser.`);
  }

  console.log(`\nPASS — live account format pinned in ${SAMPLE_PATH}`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
