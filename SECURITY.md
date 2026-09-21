# Security Policy

## What counts as a security issue here

WiFi Sapper is **offensive security tooling** — an autonomous WPA/WPA2 handshake-hunting firmware
appliance for authorized pentesting. Its capture and (operator-armed) deauth capabilities are the
intended function, **not** vulnerabilities. All use is governed by the [DISCLAIMER](DISCLAIMER.md):
authorized testing, education, and research only.

A reportable security issue is a defect **in the firmware itself** that puts its *operator* or their
data at risk — for example:

- The Maintenance dashboard leaking recovered passwords past its passphrase gate, or the shipped
  hunt/HUD/serial surfaces exposing a plaintext PSK they are designed never to show.
- The wpa-sec cloud upload/sync exposing the operator's API key — e.g. a TLS-validation or
  certificate-pinning regression that would let the key be MITM'd.
- The push webhook payload carrying a plaintext PSK off-device (it is designed to carry network
  identity only).
- The captive setup portal echoing back a provisioned secret, or provisioning becoming writable over
  the serial channel (it is designed never to be).
- Any remote-code-execution, credential-leak, or data-loss bug in the firmware.

Please do **not** report the existence of the attack features themselves (handshake capture, deauth),
and do not ask for help attacking a network you are not authorized to test.

## Supported versions

The project is in **alpha**. Only the latest `main` is supported; fixes land there.

| Version            | Supported |
| ------------------ | --------- |
| latest `main`      | ✅        |
| older commits/tags | ❌        |

## Reporting a vulnerability

Report privately — not in a public issue:

1. **Preferred:** GitHub private vulnerability reporting — this repository's
   **Security → Advisories → Report a vulnerability** tab.
2. Otherwise, contact the maintainer [@Isonimus](https://github.com/Isonimus).

Please include the affected version or commit, the affected device (currently the M5Stack Cardputer
ADV reference target), reproduction steps, and impact. There is no bug-bounty program — this is a
research/hobby project — but reports are welcome and will be credited if you would like.
