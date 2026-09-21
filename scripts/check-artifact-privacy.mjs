#!/usr/bin/env node
/**
 * Artifact privacy guard (ADR-0049) — wired into package.json (`check:artifact-privacy`) and the
 * pre-commit hook, so it runs on every commit rather than once.
 *
 * On-air verify artifacts are committed as evidence (ADR-0004), but a hunt/upload run sweeps up the
 * BSSIDs of *bystander* networks the operator does not own. A BSSID is a MAC address, and a MAC
 * address is geolocatable through public Wi-Fi databases (e.g. WiGLE) — so a committed artifact
 * carrying a real BSSID publishes where the device ran and exposes a third party's network. This
 * guard fails the commit if any file under artifacts/ contains a hex MAC-address pattern: redacted
 * evidence uses the non-hex placeholder `xx:xx:xx:xx:xx:xx`, which keeps the evidentiary structure
 * (a capture happened, N were discovered) without the identifier.
 *
 * SSID scrubbing is review-only: an arbitrary SSID string is not machine-distinguishable from a
 * placeholder, so the mechanical teeth cover BSSIDs only. Redacting both at the source — in the
 * on-air verify scripts, before they ever write an artifact — is the durable fix, tracked in the
 * LEDGER.
 */
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ARTIFACTS_DIR = 'artifacts';
// A hex MAC/BSSID: six colon-separated hex octets. The redaction placeholder `xx:xx:xx:xx:xx:xx`
// is not hex, so it never matches — that is the whole point of choosing a non-hex placeholder.
const HEX_MAC = /\b([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b/;
// Binary evidence (canvas PNG dumps) carries no greppable text identifiers; skip it.
const SKIP_EXT = /\.(png|jpg|jpeg|bin)$/i;

function walk(dir) {
    const out = [];
    for (const name of readdirSync(dir)) {
        const path = join(dir, name);
        if (statSync(path).isDirectory()) out.push(...walk(path));
        else out.push(path);
    }
    return out;
}

const offenders = [];
for (const file of walk(ARTIFACTS_DIR)) {
    if (SKIP_EXT.test(file)) continue;
    const lines = readFileSync(file, 'utf8').split('\n');
    lines.forEach((line, i) => {
        const hit = line.match(HEX_MAC);
        if (hit) offenders.push({ file, line: i + 1, mac: hit[0], text: line.trim() });
    });
}

if (offenders.length > 0) {
    console.error(`artifact-privacy: ${offenders.length} real BSSID(s) found in ${ARTIFACTS_DIR}/ — redact to xx:xx:xx:xx:xx:xx (ADR-0049):`);
    for (const o of offenders) console.error(`  ${o.file}:${o.line}  ${o.mac}  | ${o.text}`);
    process.exit(1);
}
console.log(`artifact-privacy: ok — no real BSSID in ${ARTIFACTS_DIR}/`);
