#!/bin/bash
# Build the closed parts of the wl (5 GHz + 2.4 GHz) image from files you
# download yourself from TP-Link. Nothing closed is in this repository.
#
#   GPL_TAR=~/Downloads/Archer_C1200v2_GPL.tar.gz \
#   STOCK_ZIP=~/Downloads/Archer_C1200v2_EU_211219_v1.zip  wl/build-wl.sh
#
# -> build/out-wl/wl.ko     Broadcom's wl_apsta.o (from the GPL drop) + the open
#                           shared code recompiled for 6.12 + our 2.6.36 ABI shim
# -> build/out-wl/wlut.tgz  wl, nas, eapd, nvram + uClibc from the stock firmware
#
# Where to get the two files: wl/fetch-blob.sh. Needs Docker and the SDK volume
# that build/run-build.sh creates (run that once first). Both downloads are
# checked against the hashes below; other versions are not tested.
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
GPL_TAR=${GPL_TAR:?set GPL_TAR to Archer_C1200v2_GPL.tar.gz (see wl/fetch-blob.sh)}
STOCK_ZIP=${STOCK_ZIP:?set STOCK_ZIP to the Archer C1200 v2 stock firmware .zip (see wl/fetch-blob.sh)}
GPL_SHA=01114eb1b888fa71115ecdca23d9fa74eaa325adcde6282afe6cc7af2e3e4a3b
ZIP_SHA=b3f6dbbb810f051fa75855dd925c0eb45225efa1ebdb9d9f743c266b2a1a0093
BLOB_SHA=5e5393f59494a8e43360fc367424fd6ced799d3d9312a34f7dde7b180945bab1
OUT=$H/build/out-wl

check() { # file sha
	echo "checking $(basename "$1") ..."
	echo "$2  $1" | shasum -a 256 -c - >/dev/null ||
		{ echo "!! sha256 mismatch for $1 (expected $2)" >&2; exit 1; }
}
[ -f "$GPL_TAR" ] || { echo "no file $GPL_TAR" >&2; exit 1; }
[ -f "$STOCK_ZIP" ] || { echo "no file $STOCK_ZIP" >&2; exit 1; }
check "$GPL_TAR" $GPL_SHA
check "$STOCK_ZIP" $ZIP_SHA
mkdir -p "$OUT"

docker run --rm --platform linux/amd64 \
	-v owrt-sdk-25126:/work \
	-v "$(cd "$(dirname "$GPL_TAR")" && pwd)/$(basename "$GPL_TAR"):/in/gpl.tar.gz:ro" \
	-v "$(cd "$(dirname "$STOCK_ZIP")" && pwd)/$(basename "$STOCK_ZIP"):/in/stock.zip:ro" \
	-v "$H/wl/src:/src:ro" \
	-v "$H/wl/extract-stock-userspace.sh:/extract.sh:ro" \
	-v "$OUT:/out" \
	-e BLOB_SHA=$BLOB_SHA \
	owrt-sdk-env bash -c '
set -euo pipefail
M=c1200v2_us/boards/model_brcm_bcm470x/sdk/linux-2.6.36-router-9.10.178.50/main
KDIR=$(ls -d /work/sdk/build_dir/target-*/linux-bcm53xx_generic/linux-6.12* 2>/dev/null | head -1)
[ -n "$KDIR" ] || { echo "!! no SDK in the owrt-sdk-25126 volume: run build/run-build.sh once first" >&2; exit 1; }

# 1. the GPL drop: only the paths the build needs (case-sensitive volume, not macOS)
if [ ! -f /work/c1200gpl/$M/src/wl/linux/wl_apsta.o ]; then
	echo "== unpacking the GPL drop (only src/shared, src/include, wl_apsta.o, components/shared)"
	mkdir -p /work/c1200gpl
	tar xzf /in/gpl.tar.gz -C /work/c1200gpl \
		$M/src/shared $M/src/include $M/src/wl/linux/wl_apsta.o \
		$M/components/shared/proto $M/components/shared/devctrl_if
fi
echo "$BLOB_SHA  /work/c1200gpl/$M/src/wl/linux/wl_apsta.o" | sha256sum -c - >/dev/null

# 2. wl.ko
echo "== building wl.ko"
GPL=/work/c1200gpl/$M OPENWRT=/work/sdk SRC=/src W=/work/wl-build bash /src/build.sh clean
cp /work/wl-build/wl.ko /out/wl.ko

# 3. the stock userspace
echo "== extracting wl/nas/eapd/nvram from the stock firmware"
T=$(mktemp -d)
python3 -c "import zipfile,sys
z=zipfile.ZipFile(\"/in/stock.zip\")
b=[n for n in z.namelist() if n.endswith(\".bin\")]
open(sys.argv[1],\"wb\").write(z.read(b[0]))" $T/stock.bin
mkdir -p $T/bin && ln -s /work/sdk/staging_dir/host/bin/unsquashfs4 $T/bin/unsquashfs  # the SDK has one
cd $T && PATH=$T/bin:$PATH bash /extract.sh $T/stock.bin /out/wlut.tgz >/dev/null
'
cd "$OUT" && ls -l wl.ko wlut.tgz && shasum -a 256 wl.ko wlut.tgz
echo "next: WL=1 HT=1 build/make-image.sh"
