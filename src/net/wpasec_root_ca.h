/**
 * @file wpasec_root_ca.h
 * @brief The single pinned trust anchor for wpa-sec uploads: the self-signed GTS Root R4 (ADR-0017
 *        decision #1).
 *
 * Not a CA bundle. This fixed-endpoint appliance trusts exactly one certificate — the self-signed
 * Google Trust Services GTS Root R4 — measured from the live wpa-sec.stanev.org chain on 2026-09-18:
 *
 *     leaf  CN=stanev.org   (valid 2026-07-26 → 2026-10-24, a ~90-day auto-rotated cert)
 *       └ intermediate  GTS WE1
 *           └ root  GTS Root R4
 *
 * The root the server *presents* is cross-signed by GlobalSign and expires 2028-01-28; pinning that
 * would brick uploads in ~16 months. This is the *self-signed* GTS Root R4 (subject == issuer,
 * ECDSA P-384), valid 2016-06-22 → **2036-06-22**, and `openssl verify -CAfile` against the live leaf
 * chain returns OK — so it validates the real chain end to end while lasting a decade. If wpa-sec ever
 * migrates off Google Trust Services the TLS handshake fails loud (a logged error, never a silent
 * fallback — quality bar §3) and the fix is re-measuring the chain and updating this PEM in a firmware
 * release. That maintenance obligation is tracked in LEDGER.md, citing ADR-0017.
 *
 * Device-only: consumed by uploader_wpasec.cpp via WiFiClientSecure::setCACert.
 */
#pragma once

namespace sapper {

/// Self-signed GTS Root R4, PEM. The one certificate the uploader trusts (ADR-0017 decision #1).
inline constexpr const char kWpaSecRootCaPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

}  // namespace sapper
