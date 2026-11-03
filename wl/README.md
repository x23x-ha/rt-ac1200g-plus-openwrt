# 5 GHz (and faster 2.4 GHz): Broadcom's `wl` driver

The 802.11ac core inside the BCM47189 has no open Linux driver. Broadcom's own
driver does run on it: the `wl_apsta.o` object from TP-Link's GPL release for the
Archer C1200 v2 (same SoC family), built for Linux 2.6.36, loaded into OpenWrt's
6.12 kernel through an ABI shim. It drives both radios, so 2.4 GHz moves from b43
to `wl` too.

Measured on this router (iPhone, same spot, router CPU not the limit):

| | b43 image (HT) | wl image |
|---|---|---|
| 5 GHz | no driver | 802.11ac VHT80, 866 Mbit/s link, 152 down / 211 up |
| 2.4 GHz | 32-35 down / 55-58 up | 66 down / 85 up (HT20) |

**Nothing closed is in this repository.** You download Broadcom's object (inside
TP-Link's GPL tarball) and the stock userspace tools (inside TP-Link's firmware)
yourself, and build the image on your own machine. Keep the result private: it
contains Broadcom's code, which you may not pass on.

This work builds on the Archer C1200 v2 port by Vygis1
(https://github.com/Vygis1/archer-c1200-v2-openwrt, GPL-2.0): the shim, the
netifd driver and the iwinfo backend come from there, adapted to this board and
to OpenWrt 25.12.6.

## Build

Needs Docker (x86_64 or emulated), ~20 GB of disk, and the b43 build set up once
(`build/run-build.sh`, see the main README).

```sh
sh wl/fetch-blob.sh                     # what to download from TP-Link, with hashes

GPL_TAR=~/Downloads/Archer_C1200v2_GPL.tar.gz \
STOCK_ZIP=~/Downloads/Archer_C1200v2_EU_211219_v1.zip \
  wl/build-wl.sh                        # -> build/out-wl/wl.ko + wlut.tgz (~10 min)

wl/build-iwinfo.sh                      # optional: LuCI shows the wl radios (~15 min)
build/build-kernel.sh                   # recommended: bgmac fix, see below (1-2 h)

HT=1 build/run-build.sh                 # the fallback b43 module, if not built yet
WL=1 HT=1 ZIMAGE=build/out-kernel/zImage build/make-image.sh
# -> build/images/openwrt-25.12.6-asus_rt-ac1200g-plus-squashfs-wl.trx
```

**Kernel (`build/build-kernel.sh`):** the SoC's gmac corrupts forwarded frames
with OpenWrt's 128-byte TX DMA bursts (`patches/kernel/950-…`): the receiver sees
UDP checksum errors and truncated packets, mostly for uploads from Wi-Fi through
the router. With 64-byte bursts: 0 errors, TCP upload 38-52 → 327 Mbit/s. It
applies to the b43 image too. It is a full OpenWrt source build; the script checks
that the kernel config matches the release, so the release's kernel modules load.

Flash as described in the main README (patched image, `sysupgrade` or CFE rescue).

## After flashing

- 2.4 GHz comes up as "ASUS" on channel 1 (HT20), 5 GHz as "ASUS-5G" on channel 36
  (VHT80). **Both start disabled without a key**: set one, then enable
  (LuCI → Wireless, or `uci set wireless.default_radio1.key=…; uci set
  wireless.radio1.disabled=0; uci commit wireless; wifi`).
- Encryption: WPA2-PSK (CCMP) or open. No WPA3, no multiple SSIDs per radio:
  WPA2 is done by Broadcom's `nas`, not hostapd.
- Country: the driver's own locale is `wireless.radio1.wl_country` (default
  `E0/943`, 5 GHz channels 36-64 and 100-112). The shared `country` option does
  not reach it. Check what is legal where you are.
- `iwinfo` (a ucode script in 25.12) only knows nl80211 radios; with
  `build-iwinfo.sh` LuCI and `ubus call iwinfo info '{"device":"eth2"}'` work.
- Back to b43 for 2.4 GHz: `uci set wireless.radio0.keep_b43=1` before flashing,
  or by hand: `echo b43 > /etc/modules.d/b43`, radio0 back to `type mac80211`, reboot.

## Things to know

- **`rmmod wl` hangs the router.** To stop a radio, disable it and run `wifi`;
  reboot if the module must go.
- **`wl-guard`:** if a boot with a wl radio enabled does not reach 150 s, the next
  boot disables the wl radios (a bad driver costs one reboot, not the router). So
  don't reboot within 150 s of boot, or the radios come back disabled.
- `nas`/`eapd` (WPA2) are restarted for both radios whenever one is set up; on the
  first setup after boot `eapd` exits once and is started again in the background
  within ~10 s (logged as `wl-auth`).
- `multicast_to_unicast=0` (no hairpin) on the wl interfaces is required: with it,
  every client gets disassociated after a few seconds.

## Files

| | |
|---|---|
| `fetch-blob.sh` | what to download, with sizes and hashes |
| `build-wl.sh` | Docker: GPL tarball → `wl.ko`; stock firmware → `wlut.tgz` |
| `extract-stock-userspace.sh` | pulls `wl`, `nas`, `eapd`, `nvram` + uClibc out of the stock image (Archer port, unchanged) |
| `src/build.sh`, `src/compat/` | the 2.6.36 ABI shim and the build that links it with `wl_apsta.o` |
| `files/wl-driver.uc` | netifd wireless driver for `wl` (UCI → `wl` ioctls, `nas`/`eapd`) |
| `files/mac80211-multi.uc` | `/lib/netifd/wireless/mac80211.sh`: serves both `mac80211` and `broadcom` radios |
| `files/mac80211.orig.sh` | OpenWrt's original `mac80211.sh`, called for mac80211 radios |
| `files/wl-guard` | the boot-loop guard |
| `files/90-rt-ac1200g-wl` | first-boot settings (both radios on wl, disabled, no key) |
| `iwinfo/`, `build-iwinfo.sh` | libiwinfo's `wl` backend |
