#!/bin/bash
# Build wl.ko: Broadcom's prebuilt wl_apsta.o + the SDK's open shared code
# recompiled for 6.12 + our 2.6.36 ABI shim.
#
#   ./build.sh            incremental
#   ./build.sh clean      re-copy pristine SDK sources and redo the blob rewrite
set -u

# Paths - override any of these in the environment:
#   GPL      TP-Link's GPL drop, unpacked (scripts/fetch-blob.sh prints the path)
#   OPENWRT  your OpenWrt tree, already built once for this device
#   SRC      this directory (compat/*.c, k26.h)
#   W        scratch build directory; wl.ko ends up in $W/wl.ko
GPL=${GPL:-$HOME/c1200gpl/c1200v2_us/boards/model_brcm_bcm470x/sdk/linux-2.6.36-router-9.10.178.50/main}
OPENWRT=${OPENWRT:-$HOME/openwrt}
SRC=${SRC:-$(cd "$(dirname "$0")" && pwd)}
W=${W:-$HOME/wlport/mod}
KDIR=$(ls -d $OPENWRT/build_dir/target-*/linux-bcm53xx_generic/linux-6.12* | head -1)
export STAGING_DIR=$(ls -d $OPENWRT/staging_dir/toolchain-* | head -1)
[ -d "$GPL/src/wl/linux" ] || { echo "GPL drop not found at $GPL - see scripts/fetch-blob.sh" >&2; exit 1; }
[ -n "$KDIR" ] || { echo "no built kernel in $OPENWRT/build_dir - build the image first (docs/BUILD.md)" >&2; exit 1; }
CROSS=$STAGING_DIR/bin/arm-openwrt-linux-muslgnueabi-
OBJCOPY=${CROSS}objcopy
NM=${CROSS}nm
CC=${CROSS}gcc
MAKE="make -C $KDIR M=$W ARCH=arm CROSS_COMPILE=$CROSS"

SHARED="aiutils bcmotp bcmsrom bcmutils hnd_pktpool hnd_pktq hnddma hndpmu linux_osl nicpci pcie_core sbutils siutils"

# Kernel symbols the blob imports that are safe to resolve against 6.12 as-is:
# same ABI, and they touch none of the structures whose layout changed.
KEEP="__this_module jiffies outer_cache num_physpages param_ops_int param_ops_string
      __aeabi_idiv __aeabi_idivmod __aeabi_uidiv __aeabi_uidivmod
      __aeabi_unwind_cpp_pr0 __aeabi_unwind_cpp_pr1
      memcpy memmove memset memcmp strcmp strcpy strlen strncmp strncpy strrchr
      snprintf sprintf capable schedule"

mkdir -p $W/shared $W/include $W/compat

# ---------------------------------------------------------------- sources
if [ "${1:-}" = clean ]; then
	echo "== re-copying pristine SDK sources"
	rm -f $W/shared/*.c $W/include/linux_osl.h
	for f in $SHARED; do cp $GPL/src/shared/$f.c $W/shared/; done
	cp $GPL/src/shared/*.h $W/shared/ 2>/dev/null
	for d in bcmwifi linux nvram; do
		cp -r "$GPL/src/shared/$d" "$W/shared/" 2>/dev/null
	done
	cp $GPL/src/include/*.h $W/include/
	cp -r $GPL/components/shared/proto $W/include/
	cp -r $GPL/components/shared/devctrl_if $W/include/ 2>/dev/null
	rm -f $W/shared/*.o $W/compat/*.o $W/wl.o $W/wl.ko $W/wl_blob.o $W/redefine.txt
	# also strip .applied markers so patches re-apply against fresh sources
	rm -rf $W/.patches_applied
fi
[ -f $W/wl_apsta_orig.o ] || cp $GPL/src/wl/linux/wl_apsta.o $W/wl_apsta_orig.o
cp -f $SRC/compat/*.c $SRC/compat/*.h $W/compat/

# ---------------------------------------------------------------- apply local patches
cd "$W" || exit 1
for p in $SRC/patches/*.patch; do
	[ -f "$p" ] || continue
	bn=$(basename "$p" .patch)
	if [ -f "$W/.patches_applied/$(basename "$p")" ]; then
		continue  # idempotent: skip if already applied
	fi
	echo "== applying patch: $bn"
	if ! patch -s -N -p1 < "$p"; then
		echo "ERROR: patch $bn failed to apply cleanly" >&2
		exit 1
	fi
	mkdir -p "$W/.patches_applied"
	touch "$W/.patches_applied/$(basename "$p")"
done

# ------------------------------------------------- patch the osl for k26 skbs
# The blob accesses sk_buff fields inline at 2.6.36 offsets, so the osl has to
# allocate and manipulate that same layout. Retarget it at struct k26_skb and
# at our allocators; the rest of the file is layout-agnostic.
if ! grep -q 'k26.h' $W/include/linux_osl.h; then
	echo "== patching osl for the 2.6.36 packet layout"
	sed -i '1i #include "k26.h"' $W/include/linux_osl.h
	sed -i 's/\bsk_buff\b/k26_skb/g' $W/include/linux_osl.h $W/shared/linux_osl.c
	sed -i \
	  -e 's/__dev_alloc_skb(\([^,]*\), *[^)]*)/k26_skb_alloc(\1)/g' \
	  -e 's/\bdev_alloc_skb(/k26_skb_alloc(/g' \
	  -e 's/\bdev_kfree_skb_any(/k26_skb_free(/g' \
	  -e 's/\bdev_kfree_skb(/k26_skb_free(/g' \
	  -e 's/\bskb_put(/wlc26_skb_put(/g' \
	  -e 's/\bskb_push(/wlc26_skb_push(/g' \
	  -e 's/\bskb_pull(/wlc26_skb_pull(/g' \
	  -e 's/skb_headroom((struct k26_skb \*) *\(\w*\))/k26_headroom(\1)/g' \
	  -e 's/skb_tailroom((struct k26_skb \*) *\(\w*\))/k26_tailroom(\1)/g' \
	  -e 's/pskb_copy((struct k26_skb \*)\(\w*\), *GFP_ATOMIC)/k26_skb_dup((struct k26_skb *)\1)/g' \
	  -e 's/skb_clone((struct k26_skb \*)\(\w*\), *GFP_ATOMIC)/k26_skb_dup((struct k26_skb *)\1)/g' \
	  -e 's/skb_to_sgvec(\(\w*\), *\(\w*\), *0, *\([^)]*\))/k26_skb_to_sgvec(\1, \2, \3)/g' \
	  -e 's/\bpci_map_single(/wlc26_pci_map_single(/g' \
	  -e 's/\bpci_unmap_single(/wlc26_pci_unmap_single(/g' \
	  -e 's/\bpci_map_sg(/wlc26_pci_map_sg(/g' \
	  -e 's/\bpci_read_config_dword(/wlc26_pci_read_config_dword(/g' \
	  -e 's/\bpci_write_config_dword(/wlc26_pci_write_config_dword(/g' \
	  -e 's/\bioremap_nocache(/ioremap(/g' \
	  -e 's/\bPCI_DMA_TODEVICE\b/DMA_TO_DEVICE/g' \
	  -e 's/\bPCI_DMA_FROMDEVICE\b/DMA_FROM_DEVICE/g' \
	  -e 's/(OSH_NULL, virt_to_dma(OSH_NULL, va)/(k26_default_dma_dev(), virt_to_phys(va)/g' \
	  $W/shared/linux_osl.c
	sed -i 's/\bioremap_nocache(/ioremap(/g' $W/include/linux_osl.h
	# The ACP window workaround is for Northstar's coherency port, which this
	# SoC does not have; 6.12's DMA API does the cache maintenance itself.
	sed -i '0,/^#include <osl.h>/s//#include <osl.h>\n#undef ACP_WAR_ENAB\n#define ACP_WAR_ENAB() 0\n#define ACP_WIN_LIMIT 0/' \
		$W/shared/linux_osl.c
fi
# The PKT* macros in the header need the same retargeting: left alone they
# call 6.12's skb helpers on k26 skbs, and __skb_trim() then writes len into
# the k26 cb[] (the PKTTAG) - every received frame kept its full buffer length
# (1948 bytes), which the bridge drops as oversized.
if ! grep -q 'k26_skb_trim' $W/include/linux_osl.h; then
	echo "== retargeting the PKT* skb macros in linux_osl.h"
	sed -i \
	  -e 's/__skb_trim((struct k26_skb\*)/k26_skb_trim((struct k26_skb*)/g' \
	  -e 's/\bskb_push((struct k26_skb\*)/wlc26_skb_push((struct k26_skb*)/g' \
	  -e 's/\bskb_pull((struct k26_skb\*)/wlc26_skb_pull((struct k26_skb*)/g' \
	  -e 's/\bskb_tailroom((struct k26_skb\*)(skb))/k26_tailroom((struct k26_skb*)(skb))/g' \
	  $W/include/linux_osl.h
fi
# osl_pci_bus() & co. read pdev->bus, which our synthesized 2.6.36 pci_dev
# does not have (NULL deref in si_devpath when the PCIe BCM43217 attaches).
# The shim answers from the real pci_dev; on ARM the stock BSP returned the
# PCI domain, numbered so that the 2.4 GHz radio is devpath0=pci/1/1/ -
# our bus number 1.
if ! grep -q 'k26_pci_bus' $W/shared/linux_osl.c; then
	echo "== retargeting osl_pci_bus/osl_pcie_domain/osl_pcie_bus"
	sed -i \
	  -e 's/return pci_domain_nr(((struct pci_dev \*)osh->pdev)->bus);/return k26_pci_bus(osh->pdev);/' \
	  -e 's/return ((struct pci_dev \*)osh->pdev)->bus->number;/return k26_pci_bus(osh->pdev);/' \
	  $W/shared/linux_osl.c
fi

# ------------------------------------------------------------------ Kbuild
cat > $W/Kbuild <<'KB'
obj-m := wl.o
SHARED := aiutils bcmotp bcmsrom bcmutils hnd_pktpool hnd_pktq hnddma hndpmu \
          linux_osl nicpci pcie_core sbutils siutils
wl-objs := $(addprefix shared/,$(addsuffix .o,$(SHARED))) \
           compat/compat.o compat/nvram.o compat/wlbus.o wl_blob.o
ccflags-y := -I$(src)/include -I$(src)/shared -I$(src)/compat \
  -I$(src)/shared/bcmwifi/include \
  -isystem $(shell $(CC) -print-file-name=include) \
  -DBCMDRIVER -DBCM5354 -DLINUX_HYBRID_POSTPROCESSING_REMOVAL -DROBO_SRAB -DBCMDMASGLISTOSL \
  -DBCM943217ROUTER_ACI_SCANMORECH -DBPHY_DESENSE -DAMPDU_TX_BIG_HAMMER -DWLTEST -DWL_EXPORT_CURPOWER \
  -DPPR_API -DBCMDBG_TRAP -DWLC_LOW -DWLC_HIGH -DWL_IOVF_RSDB_SET -DBCMDMA32 -DBCMNVRAMR -DPHY_HAL \
  -DPKTQ_LOG \
  -Wno-error -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion \
  -Wno-missing-prototypes -Wno-missing-declarations -Wno-unused -Wno-declaration-after-statement
# bcm_write_tlv() copies into a 2.6-era "uint8 data[1]" variable-length tail,
# which FORTIFY_SOURCE reports as a field-spanning write on every beacon build
CFLAGS_shared/bcmutils.o := -D__NO_FORTIFY
KB

# -------------------------------------------------------------- pass 1: C
# Compile the shared code and the shim first: the blob's rename list is
# "everything it imports that this code does not already define".
if [ ! -f $W/wl_blob.o ]; then
	echo 'static int wl_blob_placeholder;' | $CC -x c -c -o $W/wl_blob.o - 2>/dev/null
fi
echo "== pass 1: compiling shared code + shim"
$MAKE -k modules > $W/build1.log 2>&1
if grep -q ' error: ' $W/build1.log; then
	echo "== compile errors:"
	grep -E ' error: ' $W/build1.log | sed -E 's|^.*/mod/||' | sort -u | head -40
	exit 1
fi
echo "   objects: $(ls $W/shared/*.o $W/compat/*.o 2>/dev/null | wc -l)/16"

# ------------------------------------------------------- pass 2: the blob
if [ ! -s $W/redefine.txt ] || [ $W/wl_apsta_orig.o -nt $W/redefine.txt ]; then
	echo "== rewriting wl_apsta.o"
	$NM -u $W/wl_apsta_orig.o | awk '{print $NF}' | sort -u > /tmp/blob_u.txt
	for o in $W/shared/*.o; do $NM --defined-only $o; done |
		awk '{print $NF}' | sort -u > /tmp/shared_d.txt
	echo "$KEEP" | tr -s ' \n' '\n' | sed '/^$/d' | sort -u > /tmp/keep.txt
	comm -23 /tmp/blob_u.txt /tmp/shared_d.txt | comm -23 - /tmp/keep.txt |
		awk 'NF {print $1 " wlc26_" $1}' > $W/redefine.txt
	echo "   from shared code: $(comm -12 /tmp/blob_u.txt /tmp/shared_d.txt | wc -l)"
	echo "   redirected to the shim: $(wc -l < $W/redefine.txt)"
	printf 'init_module wl_blob_init\ncleanup_module wl_blob_exit\n' >> $W/redefine.txt

	$OBJCOPY --strip-debug \
		-R .modinfo -R __param -R .comment \
		--rename-section .init.text=.text \
		--rename-section .exit.text=.text \
		--rename-section .devinit.text=.text \
		--rename-section .devexit.text=.text \
		--rename-section .ARM.exidx.init.text=.ARM.exidx \
		--rename-section .ARM.exidx.exit.text=.ARM.exidx \
		--rename-section .ARM.exidx.devinit.text=.ARM.exidx \
		--rename-section .ARM.exidx.devexit.text=.ARM.exidx \
		--redefine-syms=$W/redefine.txt \
		$W/wl_apsta_orig.o $W/wl_blob.o || exit 1
	echo "   wl_blob.o: $(stat -c %s $W/wl_blob.o) bytes"
	rm -f $W/wl.o $W/wl.ko
fi

# ------------------------------------------------------------ pass 3: link
echo "== pass 2: linking wl.ko"
$MAKE modules > $W/build.log 2>&1
rc=$?
grep -E ' error: |undefined|Unknown symbol|No rule to make' $W/build.log |
	sed -E 's|^.*/mod/||' | sort -u | head -40

if [ -f $W/wl.ko ]; then
	echo "== wl.ko: $(stat -c %s $W/wl.ko) bytes"
	$NM -u $W/wl.ko | awk '{print $NF}' | sort -u > /tmp/ko_u.txt
	awk '{print $2}' $KDIR/Module.symvers | sort -u > /tmp/kexp.txt
	comm -23 /tmp/ko_u.txt /tmp/kexp.txt > /tmp/ko_bad.txt
	echo "== imports=$(wc -l < /tmp/ko_u.txt)  not-exported-by-kernel=$(wc -l < /tmp/ko_bad.txt)"
	[ -s /tmp/ko_bad.txt ] && { cat /tmp/ko_bad.txt | tr '\n' ' ' | fold -sw 160; echo; }
else
	echo "== no wl.ko (rc=$rc)"
fi

[ -f $W/wl.ko ] && [ ! -s /tmp/ko_bad.txt ]
