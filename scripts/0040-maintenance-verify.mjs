#!/usr/bin/env node
/**
 * scripts/0040-maintenance-verify.mjs — device verify for slice-0040 + slice-0044 (ADR-0039, ADR-0043,
 * ADR-0004 lane 3).
 *
 * Proves the runtime Maintenance-mode claims a unit test cannot: that a BOOT-held boot lands in the
 * Maintenance phase, raises the hardened SoftAP, and serves the results dashboard — while the pure
 * halves (boot-gate precedence, passphrase validation, dashboard rendering, backstop arithmetic) are
 * lane 1 (test/test_provisioning, test/test_provisioning_store, test/test_dashboard). Runs on a
 * workstation with the board attached, never in cloud CI (ADR-0004 invariant #5). Requires `npm install`.
 *
 * slice-0044 (ADR-0043) adds the write controls to the same served surface, so this one script proves
 * them too (no new verify — the rule of three, §3): the dashboard renders a POST `/resume` form and a
 * `GET /config` link, `GET /config` serves the setup form (no secret echoed), and — opt-in, because it
 * reboots the device — `POST /resume` makes the device log its resume-and-reboot line. The POST-only
 * gating of the mutating controls (§4 #23) is host-tested (test/test_dashboard).
 *
 * Precondition: flash the `cardputer_testhooks` build built with
 *   SAPPER_TEST_MAINT=1                 (forces maintenanceRequested() true — the physical BOOT-hold
 *                                        cannot be driven headlessly; a build-time hook, never serial, §4 #7)
 *   SAPPER_TEST_MAINT_PASS=<>=8 chars>  (the hardened AP passphrase to assert)
 *   SAPPER_TEST_WIFI_SSID / _PASS / _WPASEC_KEY  (a usable triad, so the gate sees the device provisioned)
 * and, for the recovered-results assertion, a device whose /cracked.bin holds at least one entry (run the
 * sync verify first, or seed the manifest on the bench).
 *
 * Machine-checkable half (serial, always): asserts `state` -> `[STATE] phase=maintenance`, captures the
 * boot `[MAINT] ssid=... url=... recovered=N` banner, and confirms the device still answers `ping`/`state`
 * while parked in Maintenance (§4 invariant #3 — a pre-engine blocking state pumps serial). Fails loud on
 * any [FATAL]/[ERROR] line.
 *
 * Eyeball/HTTP half (optional): join the workstation to the "Sapper-Maint-XXXX" AP (ADR-0055; distinct
 * from the provisioning portal's "Sapper-XXXX") using the maintenance
 * passphrase (this proves the AP is NOT open and NOT using the default "sapper-setup" password), then set
 * SAPPER_MAINT_DASHBOARD_URL=http://192.168.4.1/ to have this script fetch the dashboard and assert it is
 * HTML that lists the recovered networks — including a plaintext PSK you seeded (ADR-0039 decision 5).
 *
 * Usage: node scripts/0040-maintenance-verify.mjs [/dev/ttyACM0]
 */
import { SerialPort } from 'serialport';
import { mkdirSync, writeFileSync } from 'node:fs';

const BAUD = 115200;
const ARTIFACT_DIR = 'artifacts';
const ARTIFACT = `${ARTIFACT_DIR}/0040-maintenance.txt`;
const REPLY_TIMEOUT_MS = 4000;
const BANNER_TIMEOUT_MS = 8000;

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

/** A line-oriented view over the port, remembering the [MAINT] banner and any fatal line seen. */
class Channel {
  constructor(port) {
    this.port = port;
    this.buffer = '';
    this.waiters = [];
    this.fatalLine = null;
    this.maintLine = null;
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
      if (/^\[MAINT\] ssid=/.test(line)) this.maintLine = line;
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
}

/** Machine-checkable serial half: phase=maintenance, the [MAINT] banner, and serial still answered. */
async function verifyMaintenanceSerial(channel, observed) {
  // The [MAINT] banner is printed once at boot; catch it if this run opened the port in time.
  const bannerP = channel.waitForLine(/^\[MAINT\] ssid=/, BANNER_TIMEOUT_MS);

  // §4 #3: parked in the Maintenance serve loop, the channel must still answer.
  const pongP = channel.waitForLine(/^\[CMD\] pong/, REPLY_TIMEOUT_MS);
  await channel.send('ping');
  await pongP.catch((e) => fail(`Maintenance did not answer ping (invariant #3): ${e.message}`));

  const stateP = channel.waitForLine(/^\[STATE\] phase=maintenance\b/, REPLY_TIMEOUT_MS);
  await channel.send('state');
  const state = await stateP.catch((e) => {
    fail(`state in Maintenance not phase=maintenance: ${e.message}`);
    return null;
  });
  if (state) observed.push(state);

  const banner = channel.maintLine ?? (await bannerP.catch(() => null));
  if (banner) {
    observed.push(banner);
    if (!/\brecovered=\d+/.test(banner)) fail(`[MAINT] banner missing recovered=N: ${banner}`);
  } else {
    // Not fatal: the banner is a boot-time line this run may not have caught (native USB-CDC does not
    // survive a reset). phase=maintenance above is the machine-checkable proof of entry.
    console.log('note: did not catch the boot [MAINT] banner (open the port before boot to capture it)');
  }
}

/** Optional HTTP half: fetch the dashboard (workstation must be joined to the maintenance AP). */
async function verifyDashboardHttp(url, observed) {
  console.log(`fetching dashboard at ${url} (workstation must be joined to the maintenance AP)...`);
  let res;
  try {
    res = await fetch(url, { signal: AbortSignal.timeout(REPLY_TIMEOUT_MS) });
  } catch (e) {
    fail(`could not fetch the dashboard (is the workstation joined to the AP?): ${e.message}`);
    return;
  }
  if (!res.ok) {
    fail(`dashboard returned HTTP ${res.status}`);
    return;
  }
  const body = await res.text();
  if (!/WiFi Sapper - Maintenance/.test(body)) fail('dashboard body is not the Maintenance page');
  if (!/<th>Password<\/th>/.test(body)) fail('dashboard has no results table');
  const expectedPsk = process.env.SAPPER_MAINT_EXPECT_PSK;
  if (expectedPsk && !body.includes(expectedPsk)) {
    fail(`dashboard did not render the seeded PSK "${expectedPsk}" (ADR-0039 decision 5)`);
  }
  // slice-0044 (ADR-0043 §4 #23): the two controls are present, and resume is a POST form (not a GET
  // link, which a prefetch could fire). host-tested too (test_dashboard), asserted here end-to-end.
  if (!/action=['"]\/resume['"]/.test(body) || !/method=['"]POST['"]/i.test(body)) {
    fail('dashboard is missing the POST /resume control (slice-0044)');
  }
  if (!/href=['"]\/config['"]/.test(body)) fail('dashboard is missing the /config re-provision link');
  observed.push(`GET ${url} -> ${res.status}, ${body.length} bytes`);
  writeFileSync(`${ARTIFACT_DIR}/0040-maintenance.dashboard.html`, body);
  console.log(`wrote ${ARTIFACT_DIR}/0040-maintenance.dashboard.html`);

  // GET /config serves the setup form (a read; safe to fetch). Assert the form posts to /save and does
  // not echo a stored secret back as an input value (§4 #20 — the form model carries no secret string).
  const origin = new URL(url).origin;
  let cfg;
  try {
    cfg = await fetch(`${origin}/config`, { signal: AbortSignal.timeout(REPLY_TIMEOUT_MS) });
  } catch (e) {
    fail(`could not fetch /config (slice-0044): ${e.message}`);
    return;
  }
  const cfgBody = cfg.ok ? await cfg.text() : '';
  if (!cfg.ok || !/action=['"]\/save['"]/.test(cfgBody)) {
    fail(`GET /config did not serve the setup form (HTTP ${cfg.status})`);
  }
  const secret = process.env.SAPPER_MAINT_EXPECT_NO_ECHO;  // a stored secret that must NOT appear.
  if (secret && cfgBody.includes(secret)) {
    fail(`/config echoed a stored secret back into the form (§4 #20 violation)`);
  }
  observed.push(`GET ${origin}/config -> ${cfg.status}, ${cfgBody.length} bytes`);
}

/**
 * Opt-in (SAPPER_MAINT_DRIVE_RESUME=1): POST /resume and assert the device logs its resume-and-reboot
 * line, proving the mutating control fires end-to-end. Destructive (it reboots the board), so it is not
 * part of the default run and comes last. The POST-only gating itself is host-tested (test_dashboard).
 */
async function verifyResumeControl(channel, url, observed) {
  const origin = new URL(url).origin;
  const rebootP = channel.waitForLine(/^\[MAINT\] control requested resume/, BANNER_TIMEOUT_MS);
  console.log(`POST ${origin}/resume (this reboots the board)...`);
  try {
    await fetch(`${origin}/resume`, { method: 'POST', signal: AbortSignal.timeout(REPLY_TIMEOUT_MS) });
  } catch (e) {
    // The device may reboot before it finishes the HTTP response, so a fetch abort here is not itself a
    // failure — the serial line below is the authoritative proof the control fired.
    console.log(`note: /resume fetch did not complete cleanly (expected on reboot): ${e.message}`);
  }
  const line = await rebootP.catch((e) => {
    fail(`POST /resume did not trigger a reboot into Station (slice-0044): ${e.message}`);
    return null;
  });
  if (line) observed.push(line);
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
  console.log(`opened ${path} @ ${BAUD} — slice-0040 maintenance verify`);

  const observed = [];
  try {
    await verifyMaintenanceSerial(channel, observed);
    const url = process.env.SAPPER_MAINT_DASHBOARD_URL;
    if (url) {
      await verifyDashboardHttp(url, observed);
      // Opt-in and last: it reboots the board, ending this session (slice-0044).
      if (process.env.SAPPER_MAINT_DRIVE_RESUME === '1') await verifyResumeControl(channel, url, observed);
    } else {
      console.log('note: set SAPPER_MAINT_DASHBOARD_URL (joined to the AP) to verify the served page');
    }
    if (channel.fatalLine) fail(`device reported: ${channel.fatalLine}`);
  } finally {
    port.close();
  }

  writeFileSync(ARTIFACT, `${observed.join('\n')}\n`);
  console.log(`wrote ${ARTIFACT}`);

  if (failures.length > 0) {
    console.error(`\nFAIL (${failures.length}):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`\nPASS — Maintenance entry verified; review ${ARTIFACT}.`);
}

main().catch((err) => {
  console.error(`FATAL: ${err.message}`);
  process.exit(1);
});
