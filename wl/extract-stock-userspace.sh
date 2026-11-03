#!/bin/bash
# From the Archer C1200 v2 port (https://github.com/Vygis1/archer-c1200-v2-openwrt,
# GPL-2.0), unchanged.
# Pull the stock wl userspace out of a TP-Link Archer C1200 v2 firmware image:
# wl, nas, eapd, nvram and the uClibc they are linked against.
#
#   ./scripts/extract-stock-userspace.sh stock.bin [out.tgz]
#
# Needs unsquashfs (squashfs-tools). The result, wlut.tgz, is what
# wl/install.sh uploads to the router.
#
# Why the stock binaries: `wl` is the only tool that speaks this driver's
# private ioctls, and `nas` is the WPA2 authenticator - wpad/hostapd cannot
# drive a fullmac Broadcom radio. They are uClibc-linked, so their loader
# (/lib/ld-uClibc.so.0) goes along; it sits at a path musl does not use, so
# nothing conflicts.
set -e
IMG=${1:?usage: extract-stock-userspace.sh <stock firmware .bin> [out.tgz]}
OUT=${2:-wlut.tgz}
command -v unsquashfs >/dev/null || { echo "install squashfs-tools" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# The rootfs is a squashfs inside the image; find it rather than trusting an
# offset (it is 0x203d12 in the EU 211219 build).
OFF=$(python3 - "$IMG" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
i=d.find(b'hsqs')
print(i if i>=0 else -1)
PY
)
[ "$OFF" -ge 0 ] || { echo "no squashfs found in $IMG" >&2; exit 1; }
printf 'squashfs at 0x%x\n' "$OFF"
dd if="$IMG" of="$TMP/root.sqfs" bs=1M iflag=skip_bytes skip="$OFF" status=none
unsquashfs -q -d "$TMP/root" "$TMP/root.sqfs" >/dev/null

mkdir -p "$TMP/wlut/bin" "$TMP/wlut/lib"
for b in wl nas eapd nvram; do
	f=$(find "$TMP/root" -type f -name "$b" | head -1)
	[ -n "$f" ] || { echo "!! $b not in the image" >&2; exit 1; }
	cp "$f" "$TMP/wlut/bin/$b"
done
cp -a "$TMP/root"/lib/ld-uClibc*.so* "$TMP/root"/lib/lib*.so* "$TMP/wlut/lib/" 2>/dev/null
# libshared/libnvram/libbcmcrypto live in /usr/lib on some builds
cp -a "$TMP/root"/usr/lib/libshared.so "$TMP/root"/usr/lib/libnvram.so \
      "$TMP/root"/usr/lib/libbcmcrypto.so "$TMP/wlut/lib/" 2>/dev/null || true
chmod +x "$TMP/wlut/bin"/*
tar czf "$OUT" -C "$TMP" wlut
echo "$OUT:"; tar tzf "$OUT" | sed 's/^/  /'
