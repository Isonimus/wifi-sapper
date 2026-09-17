#!/bin/bash
# Weaken ieee80211_raw_frame_sanity_check in libnet80211.a so a deauth override can link
# and transmit management frames (ADR-0001). Ported from The Adversary, trimmed to the
# Sapper's only current chip (ESP32-S3 / Cardputer ADV); slice-8 re-adds esp32 for the
# M5StickC when that board is brought up.
#
# Idempotent: safe to run on every build (weaken_deauth_pre.py runs it as a pre-action).
# Harmless before a deauth.cpp exists — it edits a library archive symbol, nothing else.

set -e

LIBS_BASE="$HOME/.platformio/packages/framework-arduinoespressif32-libs"

# Resolve objcopy without hardcoding an absolute toolchain path (package names change across
# PlatformIO toolchain updates). Precedence:
#   1. $OBJCOPY exported by weaken_deauth_pre.py, derived from SCons $CC — the exact toolchain.
#   2. PATH lookup, for standalone manual runs.
# objcopy --weaken-symbol edits the archive symbol table only, so any xtensa objcopy works.
resolve_objcopy() {
    if [ -n "$OBJCOPY" ] && [ -x "$OBJCOPY" ]; then
        echo "$OBJCOPY"; return 0
    fi
    local FROM_PATH
    FROM_PATH=$(command -v xtensa-esp32s3-elf-objcopy || command -v xtensa-esp-elf-objcopy || true)
    if [ -n "$FROM_PATH" ]; then
        echo "$FROM_PATH"; return 0
    fi
    return 1
}

weaken_for_chip() {
    local CHIP=$1
    local LIBPATH="$LIBS_BASE/$CHIP/lib/libnet80211.a"

    echo "============================================"
    echo "Processing $CHIP..."
    echo "============================================"

    if [ ! -f "$LIBPATH" ]; then
        echo "  Warning: libnet80211.a not found at $LIBPATH"
        echo "  Skipping $CHIP (run 'pio run' first to download dependencies)"
        return 0
    fi

    CURRENT=$(nm "$LIBPATH" 2>/dev/null | grep "ieee80211_raw_frame_sanity_check" | awk '{print $2}')

    if [ "$CURRENT" = "W" ]; then
        echo "  Symbol is already weak (W) - no changes needed"
        return 0
    fi

    echo "  Current symbol state: $CURRENT (expected T for strong)"
    echo "  Weakening ieee80211_raw_frame_sanity_check symbol..."

    "$OBJCOPY_BIN" --weaken-symbol=ieee80211_raw_frame_sanity_check "$LIBPATH"

    NEW=$(nm "$LIBPATH" 2>/dev/null | grep "ieee80211_raw_frame_sanity_check" | awk '{print $2}')

    if [ "$NEW" = "W" ]; then
        echo "  Success! Symbol is now weak (W)"
    else
        echo "  Error: Symbol state is '$NEW', expected 'W'"
        return 1
    fi
}

echo "Weakening deauth symbol for ESP32-S3..."
echo ""

# Resolve objcopy once, up front — fail loud before touching any archive if none exists.
OBJCOPY_BIN=$(resolve_objcopy) || {
    echo "Error: no xtensa objcopy found — set \$OBJCOPY or add xtensa-esp32s3-elf-objcopy to PATH"
    exit 1
}
echo "Using objcopy: $OBJCOPY_BIN"
echo ""

weaken_for_chip "esp32s3"

echo ""
echo "============================================"
echo "Done! Rebuild your project to apply changes."
echo "============================================"
