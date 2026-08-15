# resetprop_if_diff <prop name> <expected value>
resetprop_if_diff() {
    local NAME="$1"
    local EXPECTED="$2"
    local CURRENT="$(resetprop "$NAME")"

    [ -z "$CURRENT" ] || [ "$CURRENT" = "$EXPECTED" ] || resetprop -n "$NAME" "$EXPECTED"
}

# resetprop_if_match <prop name> <value match string> <new value>
resetprop_if_match() {
    local NAME="$1"
    local CONTAINS="$2"
    local VALUE="$3"

    [[ "$(resetprop "$NAME")" = *"$CONTAINS"* ]] && resetprop -n "$NAME" "$VALUE"
}

# Cache compact-mode detection for this script invocation.
_RESETPROP_COMPACT=""

resetprop_supports_compact() {
    if [ -z "$_RESETPROP_COMPACT" ]; then
        if resetprop --help 2>/dev/null | grep -q compact; then
            _RESETPROP_COMPACT=1
        else
            _RESETPROP_COMPACT=0
        fi
    fi
    [ "$_RESETPROP_COMPACT" = "1" ]
}

# resetprop_apply_compact [prop]
# Re-bind a single prop context, or compact the whole table when no arg is given.
resetprop_apply_compact() {
    if ! resetprop_supports_compact; then
        return 0
    fi
    if [ -n "$1" ]; then
        resetprop -c $(resetprop -Z "$1") >/dev/null 2>&1 || true
    else
        resetprop -c >/dev/null 2>&1 || true
    fi
}

# stub for boot-time
ui_print() { return; }

sleep_pause() {
    # APatch and KernelSU needs this
    # but not KSU_NEXT, MMRL
    if [ -z "$MMRL" ] && [ -z "$KSU_NEXT" ] && { [ "$KSU" = "true" ] || [ "$APATCH" = "true" ]; }; then
        sleep 5
    fi
}

PIF_REPO="${PIF_REPO:-KOWX712/PlayIntegrityFix}"
PIF_BRANCH="${PIF_BRANCH:-inject_s}"
PIF_BOT_BRANCH="${PIF_BOT_BRANCH:-bot}"
OTA_COOLDOWN_SEC="${OTA_COOLDOWN_SEC:-86400}"

setup_tempdir() {
    TEMPDIR="${MODDIR:-/data/adb/modules/playintegrityfix}/temp"
    [ -w /sbin ] && TEMPDIR="/sbin/playintegrityfix"
    [ -w /debug_ramdisk ] && TEMPDIR="/debug_ramdisk/playintegrityfix"
    [ -w /dev ] && TEMPDIR="/dev/playintegrityfix"
    mkdir -p "$TEMPDIR"
}

acquire_pif_lock() {
    PIF_LOCK="${TEMPDIR:-/dev/playintegrityfix}.lock"
    if command -v flock >/dev/null 2>&1; then
        exec 9>"$PIF_LOCK"
        if ! flock -n 9; then
            echo "[!] another PlayIntegrityFix fetch is already running"
            return 1
        fi
    fi
    return 0
}

valid_security_patch() {
    echo "$1" | grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}$'
}

download_fail() {
    dl_domain=$(echo "$1" | awk -F[/:] '{print $4}')
    rm -rf "$TEMPDIR"
    ping -c 1 -W 5 "$dl_domain" > /dev/null 2>&1 || {
        echo "[!] Unable to connect to $dl_domain, please check your internet connection and try again"
        sleep_pause
        exit 1
    }
    conflict_module=$(ls /data/adb/modules | grep busybox)
    for i in $conflict_module; do
        echo "[!] Please remove $i and try again."
    done
    echo "[!] download failed!"
    echo "[x] bailing out!"
    sleep_pause
    exit 1
}

# Non-fatal download. Tries curl, then wget, then wget without certs (some ROMs lack CA store).
download_try() {
    local url="$1"
    local dest="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --connect-timeout 10 --max-time 20 "$url" > "$dest" && [ -s "$dest" ] && return 0
    fi
    if command -v busybox >/dev/null 2>&1; then
        busybox wget -T 10 -qO - "$url" > "$dest" && [ -s "$dest" ] && return 0
        busybox wget -T 10 --no-check-certificate -qO - "$url" > "$dest" && [ -s "$dest" ] && return 0
    fi
    return 1
}

download() { download_try "$1" "$2" || download_fail "$1"; }

# Try jsDelivr / GitHub raw / gitmirror for a file on a given branch.
# usage: download_github <branch> <path> <dest>
download_github() {
    local branch="$1"
    local path="$2"
    local dest="$3"
    download_try "https://fastly.jsdelivr.net/gh/${PIF_REPO}@${branch}/${path}" "$dest" && return 0
    download_try "https://raw.githubusercontent.com/${PIF_REPO}/${branch}/${path}" "$dest" && return 0
    download_try "https://raw.gitmirror.com/${PIF_REPO}/${branch}/${path}" "$dest" && return 0
    return 1
}
