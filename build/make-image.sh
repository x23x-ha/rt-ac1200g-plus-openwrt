#!/bin/bash
# Ready-to-flash RT-AC1200G+ image: OpenWrt ImageBuilder (Tenda AC9 profile)
# + the fixed b43.ko from run-build.sh + the device-tree patch.
# Stock OpenWrt defaults (root without password, LAN 192.168.1.1, Wi-Fi off).
#   ./make-image.sh               -> build/images/openwrt-<ver>-asus_rt-ac1200g-plus-squashfs.trx
#   FIRSTBOOT=1 ./make-image.sh   also bakes in tools/rt-ac1200g-firstboot.sh
#                                 (takes over the stock ASUS settings, see the script)
# Needs: Docker, build/out/b43-stripped.ko, python3 with `fdt` + liblzma >= 5.4
# for tools/patch-trx-dtb.py (PYTHON=... to pick the interpreter).
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
V=${OWRT_VERSION:-25.12.6}
KVER=${KVER:-6.12.112}
URL=https://downloads.openwrt.org/releases/$V/targets/bcm53xx/generic
IB=openwrt-imagebuilder-$V-bcm53xx-generic.Linux-x86_64.tar.zst
KO=$H/build/out/b43-stripped.ko
OUT=$H/build/images
NAME=openwrt-$V-asus_rt-ac1200g-plus-squashfs${FIRSTBOOT:+-firstboot}.trx

[ -f "$KO" ] || { echo "missing $KO, run build/run-build.sh first"; exit 1; }
mkdir -p "$H/build/dl" "$OUT"
cd "$H/build/dl"
[ -f "$IB" ] || curl -fLO "$URL/$IB"
curl -fsSL "$URL/sha256sums" | grep " \*$IB\$" | sed 's/ \*/  /' | shasum -a 256 -c -

# FILES overlay: copied over the rootfs after the packages are installed
F=$(mktemp -d); trap 'rm -rf "$F"' EXIT
mkdir -p "$F/lib/modules/$KVER" && cp "$KO" "$F/lib/modules/$KVER/b43.ko"
if [ -n "${FIRSTBOOT:-}" ]; then
	mkdir -p "$F/etc/uci-defaults"
	cp "$H/tools/rt-ac1200g-firstboot.sh" "$F/etc/uci-defaults/99-rt-ac1200g-firstboot"
fi
chmod -R a+rX "$F"

rm -f "$OUT"/*tenda_ac9*
docker run --rm --platform linux/amd64 \
	-v "owrt-ib-${V//./}:/work" -v "$H/build/dl:/dl:ro" -v "$F:/files:ro" -v "$OUT:/out" \
	owrt-sdk-env bash -c "set -e
		[ -d /work/ib ] || { mkdir /work/ib; tar --zstd -xf /dl/$IB -C /work/ib --strip-components=1; }
		cd /work/ib
		make image PROFILE=tenda_ac9 FILES=/files BIN_DIR=/out >/work/image.log 2>&1 \
			|| { tail -n 40 /work/image.log; exit 1; }
		tail -n 3 /work/image.log"

SRC=$(ls "$OUT"/*tenda_ac9-squashfs.trx)
"${PYTHON:-python3}" "$H/tools/patch-trx-dtb.py" "$SRC" "$OUT/$NAME"
rm -f "$OUT"/*tenda_ac9*
cd "$OUT" && shasum -a 256 "$NAME" > "$NAME.sha256" && cat "$NAME.sha256"
