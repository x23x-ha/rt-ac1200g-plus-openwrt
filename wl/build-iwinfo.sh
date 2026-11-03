#!/bin/bash
# Optional: libiwinfo with the Broadcom "wl" backend, so LuCI and `ubus call
# iwinfo` show the wl radios (channel, mode, bitrate, encryption, clients).
# OpenWrt builds libiwinfo with BACKENDS="nl80211" only; this is the same
# version (the one in 25.12.6) with BACKENDS="nl80211 wl" and the fixes from
# the Archer C1200 v2 port (wl/iwinfo/200-wl-backend-fixes.patch). Open source.
#
#   wl/build-iwinfo.sh   -> build/out-iwinfo/libiwinfo.so.20230701
#
# Needs Docker and the SDK volume from build/run-build.sh. Downloads the base
# feed (git.openwrt.org) on the first run.
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
OUT=$H/build/out-iwinfo
BASE=64900e756efa667774214f9c6b17c113222e01f7   # the 25.12.6 SDK's base feed (feeds.conf.default)
mkdir -p "$OUT"

docker run --rm --platform linux/amd64 \
	-v owrt-sdk-25126:/work \
	-v "$H/wl/iwinfo:/p:ro" \
	-v "$OUT:/out" \
	-e BASE=$BASE \
	owrt-sdk-env bash -c '
set -euo pipefail
cd /work/sdk
./scripts/feeds update base >/work/iwinfo-feeds.log 2>&1
grep -q "$BASE" feeds.conf.default || { echo "!! SDK base feed is not $BASE" >&2; exit 1; }
# the package from the base feed (same commit as the release), as a local copy
# (Makefile + the OpenWrt patch 100-multi_radio; ours goes on top as 200)
D=package/iwinfo
rm -rf $D
cp -r feeds/base/network/utils/iwinfo $D
mkdir -p $D/patches
sed -i "s/BACKENDS=\"nl80211\"/BACKENDS=\"nl80211 wl\"/" $D/Makefile
grep -q "BACKENDS=\"nl80211 wl\"" $D/Makefile
cp /p/200-wl-backend-fixes.patch $D/patches/
./scripts/feeds install libjson-c libnl-tiny libubox uci ubus lua >>/work/iwinfo-feeds.log 2>&1
make defconfig >/dev/null
for p in libjson-c libnl-tiny libubox uci ubus lua iwinfo; do
	echo "== $p"
	make package/$p/compile -j1 V=s >/work/iwinfo-$p.log 2>&1 ||
		{ tail -n 40 /work/iwinfo-$p.log; exit 1; }
done
grep -q -- "-DUSE_WL" /work/iwinfo-iwinfo.log || { echo "!! wl backend not compiled in" >&2; exit 1; }
L=$(find staging_dir build_dir -name "libiwinfo.so*" -type f | head -1)
[ -n "$L" ] || { echo "!! libiwinfo.so not found" >&2; exit 1; }
S=$(ls staging_dir/toolchain-*/bin/arm-openwrt-linux-muslgnueabi-strip)
$S --strip-unneeded -o /out/libiwinfo.so.20230701 "$L"
'
ls -l "$OUT/libiwinfo.so.20230701" && shasum -a 256 "$OUT/libiwinfo.so.20230701"
