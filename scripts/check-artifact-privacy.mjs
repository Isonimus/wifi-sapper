#!/usr/bin/env node
/**
 * Artifact privacy guard (ADR-0049, hardened per its 2026-09-21 amendments) — wired into
 * package.json (`check:artifact-privacy`) and the pre-commit hook, so it runs on every commit.
 *
 * On-air verify artifacts are committed as evidence (ADR-0004), but a hunt/upload run sweeps up the
 * identifiers of *bystander* networks the operator does not own. A BSSID is a MAC address, and a MAC
 * address is geolocatable through public Wi-Fi databases (e.g. WiGLE) — so a committed artifact
 * carrying a real BSSID (or the device's own MAC-derived SoftAP name) publishes where the device ran
 * and exposes a third party. This guard fails if any text file under <root>/artifacts contains a
 * MAC-address pattern (colon, hyphen, or Cisco-dotted) or a hex SoftAP name; redacted evidence uses
 * the non-hex placeholders `xx:xx:xx:xx:xx:xx` / `Sapper-XXXX`.
 *
 * Deliberate scope and gaps (ADR-0049 amendments; the source-redaction LEDGER item is the durable fix):
 *  - **Raw capture files (.pcap/.pcapng/.cap) hard-fail unconditionally.** Their MAC bytes are binary,
 *    not scannable text, and per §4 invariant #9 capture bytes belong only behind the CaptureSink seam
 *    (and the gitignored src/gen/), never in artifacts/ — so their mere presence is the violation.
 *  - **PNG/binary artifacts are skipped:** a screenshot renders identifiers as *pixels* no regex can
 *    read. Redaction at the verify-script source (feed placeholders into the surface before capture) is
 *    the fix.
 *  - **SSID text is review-only:** an arbitrary SSID string is not machine-distinguishable from a
 *    placeholder. Only MAC/SoftAP identifiers are mechanically enforced.
 *  - **The adr/ + slices/ corpus is NOT scanned:** it legitimately contains synthetic example MACs
 *    (`aa:bb:cc:dd:ee:ff`, `Sapper-0000`), which this guard cannot tell from a real one — so keeping
 *    real identifiers out of ADR/slice prose is a review discipline, not a mechanical check.
 *
 * The pre-commit hook runs this against the COMMITTED tree (an extracted $staged root), not the
 * working tree, so a staged-real / working-tree-clean split cannot slip a real BSSID past it
 * (stele:ADR-0018). Note a pathspec-limited `git commit -- <path>` commits working-tree content the
 * index-based hook cannot pre-see (ADR-0049 amendment 2 / F4): the durable backstop is CI scanning the
 * pushed tree, tracked in the LEDGER. Invoked with no argument it scans the working tree, for `npm run`.
 */
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { basename, join } from 'node:path';

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
// Pixel/binary evidence carries identifiers as pixels, not greppable text; skip it (documented gap).
const SKIP_EXT = /\.(png|jpg|jpeg|bin)$/i;
// Raw capture bytes must never be committed at all (§4 #9); their presence under artifacts/ is itself
// the violation, and their binary MACs would evade the text patterns anyway.
const CAPTURE_EXT = /\.(pcap|pcapng|cap)$/i;

function walk(dir) {
    const out = [];
    for (const name of readdirSync(dir)) {
        const path = join(dir, name);
        // statSync throws on a dangling symlink; that is left to propagate (fail-closed) rather than
        // being swallowed — a swallowed walk error would pass the whole commit unchecked (F2).
        if (statSync(path).isDirectory()) out.push(...walk(path));
        else out.push(path);
    }
    return out;
}

// Only a genuinely-absent artifacts/ is a clean pass (a commit before any verify ran). Any other
// error walking or reading is left to throw, so the commit fails closed rather than open.
if (!existsSync(artifactsDir)) {
    console.log(`artifact-privacy: ok — no ${artifactsDir}`);
    process.exit(0);
}

const offenders = [];
for (const file of walk(artifactsDir)) {
    if (CAPTURE_EXT.test(file)) {
        offenders.push({ file, line: 0, label: 'raw capture — never commit (§4 #9)', hit: basename(file), text: '(binary capture bytes)' });
        continue;
    }
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
