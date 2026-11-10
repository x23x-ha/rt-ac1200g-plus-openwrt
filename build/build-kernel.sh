#!/bin/bash
# The OpenWrt 25.12.6 bcm53xx kernel with patches/kernel/* (bgmac: 64-byte TX
# DMA bursts). Without it the BCM53573's gmac corrupts forwarded frames: UDP
# checksum errors and truncated packets on the receiver, mostly for Wi-Fi uploads
# through the router, but wired forwarding too. Open source; the result works
# for both the b43 and the wl image.
#
#   build/build-kernel.sh       -> build/out-kernel/zImage   (1-2 h the first time)
#   ZIMAGE=build/out-kernel/zImage HT=1 build/make-image.sh
#   FASTPATH=1 build/build-kernel.sh -> build/out-kernel-fastpath/{zImage,fastpath.ko}
#     NAT fast path from the TP-Link Archer C1200 v2 port: patches/kernel-fastpath/*
#     (323 bgmac RX hook, includes the 64-byte TX bursts; 324 RX headroom) instead
#     of patches/kernel/*, plus the kmod-fastpath package (fastpath/).
#
# Full OpenWrt source build in the Docker volume owrt-src (~15 GB). The release's
# config.buildinfo is used with CONFIG_ALL/ALL_NONSHARED off but CONFIG_ALL_KMODS
# on: kmods select kernel options, and without them the kernel .config (and so the
# module ABI) would differ from the release. The script checks that the result's
# .config is identical to the SDK's, so the release's kmods still load.
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
V=${OWRT_VERSION:-25.12.6}
FASTPATH=${FASTPATH:-0}
if [ "$FASTPATH" = 1 ]; then KP=$H/patches/kernel-fastpath; OUT=$H/build/out-kernel-fastpath
else KP=$H/patches/kernel; OUT=$H/build/out-kernel; fi
mkdir -p "$OUT"

docker run --rm --platform linux/amd64 \
	-v owrt-src:/work \
	-v owrt-sdk-25126:/sdk:ro \
	-v "$KP:/kpatches:ro" \
	-v "$H/fastpath:/fastpath:ro" \
	-v "$OUT:/out" \
	-e V="$V" -e FASTPATH="$FASTPATH" \
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
# the volume is shared between variants: drop the other variant'"'"'s patches first
for p in 950-net-bgmac-64-byte-tx-dma-bursts.patch 323-net-bgmac-add-a-receive-fast-path-hook.patch 324-net-bgmac-rx-headroom-for-wl.patch; do
	rm -f "target/linux/bcm53xx/patches-6.12/$p"
done
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
if [ "$FASTPATH" = 1 ]; then
	echo "== kmod-fastpath"
	rm -rf package/kernel/fastpath
	cp -r /fastpath package/kernel/fastpath
	grep -q "^CONFIG_PACKAGE_kmod-fastpath=m" .config || { echo CONFIG_PACKAGE_kmod-fastpath=m >> .config; make defconfig >/dev/null; }
	make package/kernel/fastpath/clean package/kernel/fastpath/compile V=s >/work/fastpath.log 2>&1 || { tail -n 40 /work/fastpath.log; exit 1; }
	cp "$(find build_dir/target-*/linux-bcm53xx_generic/fastpath -name fastpath.ko | head -n1)" /out/fastpath.ko
	find bin/targets/bcm53xx/generic/packages -name "kmod-fastpath*" -exec cp {} /out/ \;
	# the package must not have changed the kernel .config
	cmp -s "$K/.config" "$S/.config" || { echo "!! kernel .config changed by kmod-fastpath" >&2; exit 1; }
fi
'
ls -l "$OUT/zImage" && shasum -a 256 "$OUT/zImage"
