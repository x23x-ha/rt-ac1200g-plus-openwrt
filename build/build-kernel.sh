#!/bin/bash
# The OpenWrt 25.12.6 bcm53xx kernel with patches/kernel/* (bgmac: 64-byte TX
# DMA bursts). Without it the BCM53573's gmac corrupts forwarded frames: UDP
# checksum errors and truncated packets on the receiver, mostly for Wi-Fi uploads
# through the router, but wired forwarding too. Open source; the result works
# for both the b43 and the wl image.
#
#   build/build-kernel.sh       -> build/out-kernel/zImage   (1-2 h the first time)
#   ZIMAGE=build/out-kernel/zImage HT=1 build/make-image.sh
#
# Full OpenWrt source build in the Docker volume owrt-src (~15 GB). The release's
# config.buildinfo is used with CONFIG_ALL/ALL_NONSHARED off but CONFIG_ALL_KMODS
# on: kmods select kernel options, and without them the kernel .config (and so the
# module ABI) would differ from the release. The script checks that the result's
# .config is identical to the SDK's, so the release's kmods still load.
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
V=${OWRT_VERSION:-25.12.6}
OUT=$H/build/out-kernel
mkdir -p "$OUT"

docker run --rm --platform linux/amd64 \
	-v owrt-src:/work \
	-v owrt-sdk-25126:/sdk:ro \
	-v "$H/patches/kernel:/kpatches:ro" \
	-v "$OUT:/out" \
	-e V="$V" \
	owrt-sdk-env bash -c '
set -euo pipefail
cd /work
if [ ! -d openwrt ]; then
	git clone -q --branch v$V --depth 1 https://git.openwrt.org/openwrt/openwrt.git openwrt
fi
cd openwrt
git describe --tags 2>/dev/null || true
if [ ! -f .config.done ]; then
	./scripts/feeds update -a >/work/feeds.log 2>&1 && ./scripts/feeds install -a >>/work/feeds.log 2>&1
	curl -fsSL -o .config https://downloads.openwrt.org/releases/$V/targets/bcm53xx/generic/config.buildinfo
	sed -i -e "s/^CONFIG_ALL=y/# CONFIG_ALL is not set/" \
	       -e "s/^CONFIG_ALL_NONSHARED=y/# CONFIG_ALL_NONSHARED is not set/" .config
	grep -q "^CONFIG_ALL_KMODS=y" .config || echo CONFIG_ALL_KMODS=y >> .config
	make defconfig >/dev/null
	touch .config.done
fi
# sources from the OpenWrt mirror first: some upstream sites (sourceware.org) crawl
# at a few hundred bytes/s from some networks, and a slow download never fails
# over to the mirror on its own
if ! grep -q "^CONFIG_LOCALMIRROR=\"https://sources.cdn.openwrt.org\"" .config; then
	sed -i "/CONFIG_LOCALMIRROR/d" .config
	echo "CONFIG_LOCALMIRROR=\"https://sources.cdn.openwrt.org\"" >> .config
	make defconfig >/dev/null
fi
for p in /kpatches/*.patch; do
	cp "$p" target/linux/bcm53xx/patches-6.12/
	echo "patch: $(basename "$p")"
done
echo "== tools + toolchain (first run: most of the time)"
make tools/install toolchain/install -j"$(nproc)" >/work/toolchain.log 2>&1 || { tail -n 40 /work/toolchain.log; exit 1; }
echo "== kernel"
make target/linux/clean target/linux/compile -j"$(nproc)" V=s >/work/kernel.log 2>&1 || { tail -n 40 /work/kernel.log; exit 1; }
K=$(ls -d build_dir/target-*/linux-bcm53xx_generic/linux-6.12*)
for p in /kpatches/*.patch; do
	grep -q "Applying.*$(basename "$p")" /work/kernel.log || { echo "!! $(basename "$p") was not applied" >&2; exit 1; }
done
S=$(ls -d /sdk/sdk/build_dir/target-*/linux-bcm53xx_generic/linux-6.12*)
if cmp -s "$K/.config" "$S/.config"; then echo "kernel .config = SDK .config (release kmods load)"
else echo "!! kernel .config differs from the SDK: release kmods may not load" >&2; diff "$S/.config" "$K/.config" | head -20; exit 1; fi
cp "$K/arch/arm/boot/zImage" /out/zImage
'
ls -l "$OUT/zImage" && shasum -a 256 "$OUT/zImage"
