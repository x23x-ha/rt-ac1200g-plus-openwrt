#!/bin/bash
# Build kmod-b43 with the gain-table patch in the 25.12.6 SDK.
# SDK + build tree live in the Docker volume owrt-sdk-25126 (case-sensitive).
#   ./run-build.sh          -> build/out/        (816 only: the real fix)
#   DEBUG=1 ./run-build.sh  -> build/out-debug/  (816 + patches/mac80211/brcm-debug/*)
#   TSSI=1 ./run-build.sh   -> build/out-tssi/   (clean set + patches/mac80211/brcm-tssi/*)
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
OUT=$H/build/out
EXTRA=()
if [ -n "${DEBUG:-}" ]; then
	OUT=$H/build/out-debug
	EXTRA=(-v "$H/patches/mac80211/brcm-debug:/patches-extra:ro")
fi
if [ -n "${TSSI:-}" ]; then   # clean set + 819 TSSI mux test: build/out-tssi/
	OUT=$H/build/out-tssi
	EXTRA=(-v "$H/patches/mac80211/brcm-tssi:/patches-extra:ro")
fi
if [ -n "${SKIP_816:-}" ]; then   # stock tables + debug: build/out-debug-stock/
	OUT=$H/build/out-debug-stock
	EXTRA+=(-e SKIP_816=1)
fi
mkdir -p "$OUT"
docker run --rm --platform linux/amd64 \
	-v owrt-sdk-25126:/work \
	-v "$H/build/dl:/dl:ro" \
	-v "$H/patches/mac80211/brcm:/patches:ro" \
	${EXTRA[@]+"${EXTRA[@]}"} \
	-v "$H/build/build-b43.sh:/build-b43.sh:ro" \
	-v "$OUT:/out" \
	owrt-sdk-env bash -c '/build-b43.sh && cd /work/sdk &&
		K=$(find build_dir -path "*backports-*" -name b43.ko | head -1) &&
		cp -v "$K" /out/b43.ko &&
		$(ls staging_dir/toolchain-*/bin/*-objcopy | head -1) --strip-unneeded \
			--remove-section=.comment /out/b43.ko /out/b43-stripped.ko &&
		cp -v $(find bin -name "kmod-b43*.apk") /out/'
