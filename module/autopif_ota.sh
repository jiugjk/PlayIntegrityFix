#!/bin/sh

PATH=/data/adb/ap/bin:/data/adb/ksu/bin:/data/adb/magisk:/data/data/com.termux/files/usr/bin:$PATH
MODDIR=/data/adb/modules/playintegrityfix

. "$MODDIR/common_func.sh"

setup_tempdir
OTA_DIR="$TEMPDIR/ota"
OTA_STAMP="$MODDIR/autopif_ota.stamp"
mkdir -p "$OTA_DIR"

# Skip if we already checked recently (default 24h). Not a boot path.
if [ -f "$OTA_STAMP" ]; then
    now="$(date +%s 2>/dev/null)"
    prev="$(date -r "$OTA_STAMP" +%s 2>/dev/null)"
    if [ -z "$prev" ]; then
        prev="$(busybox stat -c %Y "$OTA_STAMP" 2>/dev/null)"
    fi
    if [ -n "$now" ] && [ -n "$prev" ] && [ $((now - prev)) -lt "$OTA_COOLDOWN_SEC" ]; then
        exit 0
    fi
fi

acquire_pif_lock || exit 0

script_looks_valid() {
    [ -s "$1" ] || return 1
    grep -q "^#!/bin/sh" "$1" || return 1
    grep -q "PlayIntegrityFix" "$1" || return 1
    grep -q "set_random_beta" "$1" || return 1
    grep -q "FINGERPRINT=" "$1" || return 1
    size="$(wc -c < "$1" | tr -d ' ')"
    [ -n "$size" ] && [ "$size" -gt 1000 ] && [ "$size" -lt 200000 ]
}

fetch_autopif() {
    if ! download_github "$PIF_BRANCH" "module/autopif.sh" "$OTA_DIR/temp_autopif.sh"; then
        return 1
    fi
    if ! script_looks_valid "$OTA_DIR/temp_autopif.sh"; then
        echo "[!] OTA payload rejected"
        return 1
    fi

    curhash="$(busybox crc32 "$MODDIR/autopif.sh" 2>/dev/null || cat "$MODDIR/autopif.sh" | busybox crc32)"
    newhash="$(busybox crc32 "$OTA_DIR/temp_autopif.sh" 2>/dev/null || cat "$OTA_DIR/temp_autopif.sh" | busybox crc32)"

    if [ -n "$newhash" ] && [ ! "$newhash" = "$curhash" ]; then
        cat "$OTA_DIR/temp_autopif.sh" > "$MODDIR/autopif.sh.tmp" && mv -f "$MODDIR/autopif.sh.tmp" "$MODDIR/autopif.sh"
        chmod +x "$MODDIR/autopif.sh"
        echo "[+] autopif has been updated"
    fi
    return 0
}

if fetch_autopif; then
    touch "$OTA_STAMP"
else
    echo "[!] OTA failed, skipping autopif update."
fi

rm -rf "$OTA_DIR"
