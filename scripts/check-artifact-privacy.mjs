#!/usr/bin/env node
/**
 * Artifact privacy guard (ADR-0049, hardened per its 2026-09-21 amendment) — wired into
 * package.json (`check:artifact-privacy`) and the pre-commit hook, so it runs on every commit.
 *
 * On-air verify artifacts are committed as evidence (ADR-0004), but a hunt/upload run sweeps up the
 * identifiers of *bystander* networks the operator does not own. A BSSID is a MAC address, and a MAC
 * address is geolocatable through public Wi-Fi databases (e.g. WiGLE) — so a committed artifact
 * carrying a real BSSID (or the device's own MAC-derived SoftAP name) publishes where the device ran
 * and exposes a third party. This guard fails if any text file under <root>/artifacts contains a
 * MAC-address pattern (colon, hyphen, or Cisco-dotted) or a hex SoftAP name; redacted evidence uses
 * the non-hex placeholders `xx:xx:xx:xx:xx:xx` / `Sapper-XXXX`, which keep the evidentiary structure
 * without the identifier.
 *
 * Two known, deliberate gaps (ADR-0049 amendment; both tracked in the LEDGER as the source-redaction
 * item):
 *  - PNG/binary artifacts are skipped: a screenshot of the HUD renders identifiers as *pixels*, which
 *    no regex can catch. The real fix is redaction in the verify scripts (feed placeholder identifiers
 *    into the surface before capture), so nothing real reaches disk.
 *  - SSID *text* is review-only: an arbitrary SSID string is not machine-distinguishable from a
 *    placeholder. Only MAC/SoftAP identifiers are mechanically enforced here.
 *
 * The pre-commit hook runs this against the COMMITTED tree (an extracted $staged root), not the
 * working tree, so a staged-real / working-tree-clean split cannot slip a real BSSID past it
 * (stele:ADR-0018). Invoked with no argument it scans the working tree, for `npm run` use.
 */
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const root = process.argv[2] || '.';
const artifactsDir = join(root, 'artifacts');

// Identifier patterns. The redaction placeholders (`xx:xx:xx:xx:xx:xx`, `Sapper-XXXX`) are non-hex
// where the pattern demands hex, so they never match — that is the point of choosing them.
const PATTERNS = [
    { label: 'BSSID (colon)', re: /\b([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b/ },
    { label: 'BSSID (hyphen)', re: /\b([0-9a-fA-F]{2}-){5}[0-9a-fA-F]{2}\b/ },
    { label: 'BSSID (dotted)', re: /\b([0-9a-fA-F]{4}\.){2}[0-9a-fA-F]{4}\b/ },
    // The SoftAP name is `Sapper-%02X%02X` (four hex digits, no colons) — invisible to the MAC
    // patterns above, and itself a stable, device-identifying string.
    { label: 'SoftAP name', re: /\bSapper-[0-9A-Fa-f]{4}\b/ },
];
// Binary evidence (canvas PNG dumps) carries identifiers as pixels, not greppable text; skip it —
// a deliberate, documented gap (see the header and ADR-0049 amendment), not an oversight.
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

let artifacts;
try {
    artifacts = walk(artifactsDir);
} catch {
    // No artifacts/ in this tree (e.g. a commit before any verify ran): nothing to guard.
    console.log(`artifact-privacy: ok — no ${artifactsDir}`);
    process.exit(0);
}

const offenders = [];
for (const file of artifacts) {
    if (SKIP_EXT.test(file)) continue;
    const lines = readFileSync(file, 'utf8').split('\n');
    lines.forEach((line, i) => {
        for (const { label, re } of PATTERNS) {
            const hit = line.match(re);
            if (hit) offenders.push({ file, line: i + 1, label, hit: hit[0], text: line.trim() });
        }
    });
}

if (offenders.length > 0) {
    console.error(`artifact-privacy: ${offenders.length} real identifier(s) found under ${artifactsDir} — redact before commit (ADR-0049):`);
    for (const o of offenders) console.error(`  ${o.file}:${o.line}  [${o.label}] ${o.hit}  | ${o.text}`);
    process.exit(1);
}
console.log(`artifact-privacy: ok — no real BSSID/SoftAP identifier under ${artifactsDir}`);
