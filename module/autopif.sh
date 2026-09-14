#!/bin/sh

PATH=/data/adb/ap/bin:/data/adb/ksu/bin:/data/adb/magisk:/data/data/com.termux/files/usr/bin:$PATH
MODDIR=/data/adb/modules/playintegrityfix
version=$(grep "^version=" $MODDIR/module.prop | sed 's/version=//g')

. $MODDIR/common_func.sh

setup_tempdir
cd "$TEMPDIR" || exit 1
acquire_pif_lock || exit 1
trap 'rm -rf "$TEMPDIR"' EXIT

echo "[+] PlayIntegrityFix $version"
echo "[+] $(basename "$0")"
printf "\n\n"

set_random_beta() {
	if [ "$(echo "$MODEL_LIST" | wc -l)" -ne "$(echo "$PRODUCT_LIST" | wc -l)" ]; then
		echo "Warning: MODEL_LIST and PRODUCT_LIST have different lengths, using Pixel 6 fallback"
		MODEL="Pixel 6"
		PRODUCT="oriole_beta"
	else
		count=$(echo "$MODEL_LIST" | wc -l)
		if [ "$count" -le 0 ]; then
			echo "Warning: empty device list, using Pixel 6 fallback"
			MODEL="Pixel 6"
			PRODUCT="oriole_beta"
			return
		fi
		rand_index=$(( $$ % count ))
		MODEL=$(echo "$MODEL_LIST" | sed -n "$((rand_index + 1))p")
		PRODUCT=$(echo "$PRODUCT_LIST" | sed -n "$((rand_index + 1))p")
	fi
}

load_lists_from_json() {
	[ -s "$1" ] || return 1
	MODEL_LIST="$(grep -o '"model": *"[^"]*"' "$1" | sed 's/.*"\([^"]*\)"$/\1/')"
	PRODUCT_LIST="$(grep -o '"product": *"[^"]*"' "$1" | sed 's/.*"\([^"]*\)"$/\1/')"
	[ -n "$MODEL_LIST" ] && [ -n "$PRODUCT_LIST" ]
}

get_model_product_list() {
	printf "{\"model\":["
	count=0
	total=$(echo "$MODEL_LIST" | wc -l)
	echo "$MODEL_LIST" | while read -r model; do
		count=$((count + 1))
		printf "\"%s\"" "$model"
		[ $count -lt $total ] && printf ","
	done
	printf "],\"product\":["
	count=0
	total=$(echo "$PRODUCT_LIST" | wc -l)
	echo "$PRODUCT_LIST" | while read -r product; do
		count=$((count + 1))
		printf "\"%s\"" "$product"
		[ $count -lt $total ] && printf ","
	done
	printf "]}"

	rm -rf "$TEMPDIR"
	exit 0
}

scrape_device_lists() {
	download https://developer.android.com/about/versions PIXEL_VERSIONS_HTML
	LATEST_BETA=$(grep -B4 -A2 'data-icon=\"preview' PIXEL_VERSIONS_HTML | grep -o 'href="/about/versions/.*[0-9]"' | cut -d\" -f2)
	[ "$LATEST_BETA" ] || LATEST_BETA=$(grep -oE 'href="/about/versions/[0-9]{2}"' PIXEL_VERSIONS_HTML | cut -d\" -f2 | sort -ru | head -n1)
	[ -n "$LATEST_BETA" ] || return 1
	download "https://developer.android.com$LATEST_BETA" PIXEL_LATEST_HTML

	FI_URL="https://developer.android.com$(grep -o 'href=".*download.*"' PIXEL_LATEST_HTML | cut -d\" -f2 | sort -ru | head -n1)"
	download "$FI_URL" PIXEL_FI_HTML
	OTA_URL="https://developer.android.com$(grep -o 'href=".*download-ota.*"' PIXEL_LATEST_HTML | cut -d\" -f2 | sort -ru | head -n1)"
	download "$OTA_URL" PIXEL_OTA_HTML
	SRC=FI; [ "$(grep 'tr id=' PIXEL_FI_HTML | sed 's;.*<tr id="\(.*\)">.*;\1;' | wc -w)" -lt "$(grep 'tr id=' PIXEL_OTA_HTML | sed 's;.*<tr id="\(.*\)">.*;\1;' | wc -w)" ] && SRC=OTA

	MODEL_LIST="$(grep -A1 'tr id=' PIXEL_${SRC}_HTML | grep 'td' | sed 's;.*<td>\(.*\)</td>.*;\1;')"
	PRODUCT_LIST="$(grep 'tr id=' PIXEL_${SRC}_HTML | sed 's;.*<tr id="\(.*\)">.*;\1_beta;')"
	[ -n "$MODEL_LIST" ] && [ -n "$PRODUCT_LIST" ]
}

scrape_device_prop() {
	DEVICE="$(echo "$PRODUCT" | sed 's/_beta//')"
	download https://flash.android.com PIXEL_FLASH_HTML
	FLASH_KEY=$(grep -o '<body data-client-config=.*' PIXEL_FLASH_HTML | cut -d\; -f2 | cut -d\& -f1)
	if command -v curl > /dev/null 2>&1; then
		curl -fsSL --connect-timeout 10 --max-time 20 -H "Referer: https://flash.android.com" \
			"https://content-flashstation-pa.googleapis.com/v1/builds?product=$PRODUCT&key=$FLASH_KEY" \
			> PIXEL_STATION_JSON || return 1
	else
		busybox wget -T 10 --header "Referer: https://flash.android.com" -qO - \
			"https://content-flashstation-pa.googleapis.com/v1/builds?product=$PRODUCT&key=$FLASH_KEY" \
			> PIXEL_STATION_JSON || return 1
	fi
	busybox tac PIXEL_STATION_JSON | busybox grep -m1 -A13 '"canary": true' > PIXEL_CANARY_JSON
	ID="$(grep 'releaseCandidateName' PIXEL_CANARY_JSON | cut -d\" -f4)"
	INCREMENTAL="$(grep 'buildId' PIXEL_CANARY_JSON | cut -d\" -f4)"
	FINGERPRINT="google/$PRODUCT/$DEVICE:CANARY/$ID/$INCREMENTAL:user/release-keys"
	if download_try https://source.android.com/docs/security/bulletin/pixel PIXEL_SECBULL_HTML; then
		CANARY_ID="$(grep '"id"' PIXEL_CANARY_JSON | sed -e 's;.*canary-\(.*\)".*;\1;' -e 's;^\(.\{4\}\);\1-;')"
		SECURITY_PATCH="$(grep "<td>$CANARY_ID" PIXEL_SECBULL_HTML | sed 's;.*<td>\(.*\)</td>;\1;')"
	fi
	[ -n "$ID" ] && [ -n "$INCREMENTAL" ]
}

# Prefer the daily bot/ props (same source as WebUI GitHub fetch): 1-2 small files.
# Live HTML scrape is only a fallback when the CDN/GitHub path fails.
USED_BOT=0
if download_github "$PIF_BOT_BRANCH" "device_list.json" DEVICE_LIST_JSON && load_lists_from_json DEVICE_LIST_JSON; then
	USED_BOT=1
	echo "- Loaded device list from GitHub"
else
	echo "- GitHub device list unavailable, scraping developer.android.com ..."
	scrape_device_lists || {
		echo "! Failed to get device list"
		exit 1
	}
fi

if [ "$1" = "--list" ] || [ "$1" = "-l" ]; then
	get_model_product_list
fi

echo "- Selecting Pixel Canary device ..."
if [ -z "$PRODUCT" ] || ! echo "$PRODUCT_LIST" | grep -qxF "$PRODUCT"; then
	set_random_beta
fi
# Keep MODEL in sync with the selected product when lists came from JSON.
if [ -n "$PRODUCT" ] && [ -n "$PRODUCT_LIST" ]; then
	line=$(echo "$PRODUCT_LIST" | grep -nxF "$PRODUCT" | head -n1 | cut -d: -f1)
	if [ -n "$line" ]; then
		synced=$(echo "$MODEL_LIST" | sed -n "${line}p")
		[ -n "$synced" ] && MODEL="$synced"
	fi
fi
echo "$MODEL ($PRODUCT)"

if [ "$USED_BOT" = "1" ] && download_github "$PIF_BOT_BRANCH" "device_prop/${PRODUCT}.prop" BOT_DEVICE_PROP; then
	FINGERPRINT="$(grep "^FINGERPRINT=" BOT_DEVICE_PROP | cut -d= -f2-)"
	[ -z "$MODEL" ] && MODEL="$(grep "^MODEL=" BOT_DEVICE_PROP | cut -d= -f2-)"
	SECURITY_PATCH="$(grep "^SECURITY_PATCH=" BOT_DEVICE_PROP | cut -d= -f2-)"
	echo "- Fetched pif fields from GitHub"
else
	if [ "$USED_BOT" = "1" ]; then
		echo "- GitHub device prop unavailable, scraping flash.android.com ..."
	fi
	scrape_device_prop || {
		echo "! Failed to get pif.prop"
		exit 1
	}
fi

if [ -z "$FINGERPRINT" ]; then
	echo "! Failed to get pif.prop"
	exit 1
fi

if ! valid_security_patch "$SECURITY_PATCH"; then
	echo "! Failed to determine exact security patch level"
	old_patch="$(grep "^SECURITY_PATCH=" /data/adb/pif.prop "$MODDIR/pif.prop" 2>/dev/null | cut -d= -f2 | head -n1)"
	if valid_security_patch "$old_patch"; then
		echo "- Keeping previous security patch $old_patch"
		SECURITY_PATCH="$old_patch"
	else
		echo "- Leaving SECURITY_PATCH empty rather than writing a guessed date"
		SECURITY_PATCH=""
	fi
fi

# Preserve previous setting
pifProp="$MODDIR/pif.prop"
[ -f "/data/adb/pif.prop" ] && pifProp="/data/adb/pif.prop"
spoofConfig="spoofBuild spoofProps spoofProvider spoofSignature spoofVendingBuild spoofVendingSdk DEBUG"
for config in $spoofConfig; do
	if grep -q "^${config}=true" "$pifProp"; then
		eval "$config=true"
	else
		eval "$config=false"
	fi
done

echo "- Dumping values to pif.prop ..."
echo ""
cat <<EOF | tee pif.prop
FINGERPRINT=$FINGERPRINT
MANUFACTURER=Google
MODEL=$MODEL
SECURITY_PATCH=$SECURITY_PATCH
spoofBuild=$spoofBuild
spoofProps=$spoofProps
spoofProvider=$spoofProvider
spoofSignature=$spoofSignature
spoofVendingBuild=$spoofVendingBuild
spoofVendingSdk=$spoofVendingSdk
DEBUG=$DEBUG
EOF

cat "$TEMPDIR/pif.prop" > /data/adb/pif.prop.tmp && mv -f /data/adb/pif.prop.tmp /data/adb/pif.prop
echo ""
echo "- new pif.prop saved to /data/adb/pif.prop"

if [ -e "/data/adb/tricky_store/pif_auto_security_patch" ]; then
	sh "$MODDIR/security_patch.sh"
else
	rm -f $MODDIR/system.prop
fi

echo "- Cleaning up ..."
rm -rf "$TEMPDIR"

for i in $(busybox pidof com.google.android.gms.unstable com.android.vending); do
	echo "- Killing pid $i"
	kill -9 "$i"
done

echo "- Done!"
sleep_pause
