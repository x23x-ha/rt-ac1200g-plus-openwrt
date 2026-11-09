#!/bin/bash
# Runs inside owrt-sdk-env (see run-build.sh). /work is a Docker volume
# (case-sensitive), /dl and /patches are read-only bind mounts from the Mac.
set -euo pipefail
SDK_TAR=/dl/openwrt-sdk-25.12.6-bcm53xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64.tar.zst
OWRT_COMMIT=981c978eff   # r33311-981c978eff = the 25.12.6 release build
PATCH=/patches/816-b43-add-2GHz-EPA-TX-gain-table-for-radio-2057-rev14.patch

cd /work
if [ ! -d sdk ]; then
	tar --zstd -xf "$SDK_TAR"
	mv openwrt-sdk-25.12.6-* sdk
fi
cd sdk

# mac80211 package from the exact release commit
if [ ! -d package/kernel/mac80211 ]; then
	rm -rf /work/owrt
	git clone -q --filter=blob:none --no-checkout https://github.com/openwrt/openwrt.git /work/owrt
	git -C /work/owrt sparse-checkout set package/kernel/mac80211
	git -C /work/owrt checkout -q "$OWRT_COMMIT"
	mkdir -p package/kernel
	cp -a /work/owrt/package/kernel/mac80211 package/kernel/
fi
# our patches: 816+ (clear stale ones first); /patches-extra = optional debug set
B=package/kernel/mac80211/patches/brcm
# reset the patch dir to the pristine 25.12.6 one (drops every earlier set,
# restores OpenWrt's own 813 that the HT sets replace)
rm -rf "$B"
cp -a /work/owrt/package/kernel/mac80211/patches/brcm "$B"
if [ -n "${SKIP_816:-}" ]; then install -m 644 /patches/81[7-9]-*.patch package/kernel/mac80211/patches/brcm/ 2>/dev/null || true; else install -m 644 /patches/*.patch package/kernel/mac80211/patches/brcm/; fi
if [ -d /patches-extra ]; then
	install -m 644 /patches-extra/*.patch package/kernel/mac80211/patches/brcm/
fi
ls package/kernel/mac80211/patches/brcm/ | grep -E '^8(1[3-9]|[2-8][0-9])-'

if [ ! -f .config.min ]; then
	# SDK Config-build.in declares every package the release built as a
	# prompt-less "tristate / default m", so .config can't switch them off.
	# Drop those defaults except b43 + mac80211/cfg80211; other deps come
	# back through the package definitions' selects.
	[ -f Config-build.in.orig ] || cp Config-build.in Config-build.in.orig
	awk '/^config /{sym=$2}
	     /^\tdefault m$/ && sym ~ /^PACKAGE_/ &&
	        sym !~ /^PACKAGE_kmod-(b43|mac80211|cfg80211)$/ {next}
	     {print}' Config-build.in.orig > Config-build.in
	rm -f .config .config.old
	printf '%s\n' '# CONFIG_ALL is not set' '# CONFIG_ALL_KMODS is not set' \
		'# CONFIG_ALL_NONSHARED is not set' 'CONFIG_PACKAGE_kmod-b43=m' > .config
	make defconfig >/dev/null
	touch .config.min
fi
# b43 firmware: 784.2 (broadcom-wl 6.30.163.46) with FW630=1, else OpenWrt's 666.2 default
if [ -n "${FW630:-}" ]; then
	sed -i -e 's/^CONFIG_B43_FW_5_100_138=y/# CONFIG_B43_FW_5_100_138 is not set/' \
		-e 's/^# CONFIG_B43_FW_6_30 is not set/CONFIG_B43_FW_6_30=y/' .config
else
	sed -i -e 's/^# CONFIG_B43_FW_5_100_138 is not set/CONFIG_B43_FW_5_100_138=y/' \
		-e 's/^CONFIG_B43_FW_6_30=y/# CONFIG_B43_FW_6_30 is not set/' .config
fi
grep -E '^CONFIG_PACKAGE_kmod-.*=m' .config
grep -E 'B43_' .config || true
[ -n "${CONFIG_ONLY:-}" ] && exit 0

# PREPARE_ONLY=1: unpack + apply patches only (quick check for patch conflicts)
if [ -n "${PREPARE_ONLY:-}" ]; then
	make package/kernel/mac80211/{clean,prepare} V=s > /out/prepare.log 2>&1 \
		|| { grep -nE "^Applying|FAILED|failed|Hunk|can.t find|Reversed" /out/prepare.log | tail -n 40; exit 1; }
	grep -cE "^Applying" /out/prepare.log; exit 0
fi

# full log: build/out/make.log on the Mac
make package/kernel/mac80211/{clean,compile} -j"$(nproc)" V=s > /out/make.log 2>&1 \
	|| { tail -n 60 /out/make.log; exit 1; }
tail -n 15 /out/make.log
find bin build_dir -name 'b43.ko' -o -name 'kmod-b43*.apk' | grep -v '/ipkg-' || true
