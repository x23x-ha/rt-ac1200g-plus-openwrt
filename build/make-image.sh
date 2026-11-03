#!/bin/bash
# Ready-to-flash RT-AC1200G+ image: OpenWrt ImageBuilder (Tenda AC9 profile)
# + the fixed b43.ko from run-build.sh + the device-tree patch.
# Stock OpenWrt defaults (root without password, LAN 192.168.1.1, Wi-Fi off).
#   ./make-image.sh               -> build/images/openwrt-<ver>-asus_rt-ac1200g-plus-squashfs.trx
#   FIRSTBOOT=1 ./make-image.sh   also bakes in tools/rt-ac1200g-firstboot.sh
#                                 (takes over the stock ASUS settings, see the script)
#   WL=1 ./make-image.sh          both radios on Broadcom's wl driver (5 GHz works);
#                                 needs build/out-wl/ from wl/build-wl.sh (built from
#                                 your own TP-Link downloads, see wl/README.md).
#                                 Optional: build/out-iwinfo/ (wl/build-iwinfo.sh) for
#                                 LuCI's Wi-Fi status, ZIMAGE=<file> for another kernel.
#                                 The result contains Broadcom's code: keep it private.
# Needs: Docker, build/out[-ht]/b43-stripped.ko, python3 with `fdt` + liblzma >= 5.4
# for tools/patch-trx-dtb.py (PYTHON=... to pick the interpreter).
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
V=${OWRT_VERSION:-25.12.6}
KVER=${KVER:-6.12.112}
URL=https://downloads.openwrt.org/releases/$V/targets/bcm53xx/generic
IB=openwrt-imagebuilder-$V-bcm53xx-generic.Linux-x86_64.tar.zst
# HT=1: take the 802.11n module from build/out-ht (HT=1 ./run-build.sh)
KO=${KO:-$H/build/out${HT:+-ht}/b43-stripped.ko}
OUT=$H/build/images
NAME=openwrt-$V-asus_rt-ac1200g-plus-squashfs${WL:+-wl}${FIRSTBOOT:+-firstboot}.trx

[ -f "$KO" ] || { echo "missing $KO, run build/run-build.sh first"; exit 1; }
WLO=$H/build/out-wl
if [ -n "${WL:-}" ]; then
	for f in "$WLO/wl.ko" "$WLO/wlut.tgz"; do
		[ -f "$f" ] || { echo "missing $f, run wl/build-wl.sh first (wl/README.md)"; exit 1; }
	done
fi
mkdir -p "$H/build/dl" "$OUT"
cd "$H/build/dl"
[ -f "$IB" ] || curl -fLO "$URL/$IB"
curl -fsSL "$URL/sha256sums" | grep " \*$IB\$" | sed 's/ \*/  /' | shasum -a 256 -c -

# FILES overlay: copied over the rootfs after the packages are installed
F=$(mktemp -d); trap 'rm -rf "$F"' EXIT
mkdir -p "$F/lib/modules/$KVER" && cp "$KO" "$F/lib/modules/$KVER/b43.ko"
if [ -n "${WL:-}" ]; then   # see wl/README.md for what each piece does
	W=$H/wl/files
	mkdir -p "$F/usr/lib/wl" "$F/usr/libexec/wl" "$F/lib/netifd/wireless" \
		"$F/etc/init.d" "$F/etc/uci-defaults" "$F/etc/modules.d"
	cp "$WLO/wl.ko" "$F/usr/lib/wl/wl.ko"
	tar xzf "$WLO/wlut.tgz" -C "$F/usr/lib/wl" --strip-components=1     # bin/ lib/
	cp "$F/usr/lib/wl/lib/ld-uClibc.so.0" "$F/lib/ld-uClibc.so.0"        # the tools' ELF interpreter
	cp "$W/wl-driver.uc" "$F/usr/libexec/wl/wl-driver.uc"
	cp "$W/mac80211.orig.sh" "$F/usr/libexec/wl/mac80211.orig.sh"
	cp "$W/mac80211-multi.uc" "$F/lib/netifd/wireless/mac80211.sh"
	cp "$W/wl-guard" "$F/etc/init.d/wl-guard"
	cp "$W/90-rt-ac1200g-wl" "$F/etc/uci-defaults/90-rt-ac1200g-wl"
	# b43 is not loaded: the 2.4 GHz chip runs on wl (wireless.radio0.keep_b43=1 brings it back)
	printf '# b43 not loaded: the BCM43217 (2.4 GHz) runs on wl.\n# Back to b43: put the line  b43  here and reboot.\n' > "$F/etc/modules.d/b43"
	if [ -f "$H/build/out-iwinfo/libiwinfo.so.20230701" ]; then
		mkdir -p "$F/usr/lib"
		cp "$H/build/out-iwinfo/libiwinfo.so.20230701" "$F/usr/lib/libiwinfo.so.20230701"
	fi
	chmod 755 "$F/usr/libexec/wl/"* "$F/lib/netifd/wireless/mac80211.sh" \
		"$F/etc/init.d/wl-guard" "$F/etc/uci-defaults/90-rt-ac1200g-wl" "$F/usr/lib/wl/bin/"*
fi
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
"${PYTHON:-python3}" "$H/tools/patch-trx-dtb.py" ${ZIMAGE:+--zimage "$ZIMAGE"} "$SRC" "$OUT/$NAME"
rm -f "$OUT"/*tenda_ac9*
cd "$OUT" && shasum -a 256 "$NAME" > "$NAME.sha256" && cat "$NAME.sha256"
