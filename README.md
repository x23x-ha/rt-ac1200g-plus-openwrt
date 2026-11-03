# OpenWrt on the ASUS RT-AC1200G+

Running OpenWrt 25.12 on the ASUS RT-AC1200G+ (BCM47189), which has no
official OpenWrt support, and fixing its 2.4 GHz radio in the `b43` driver.

What works:

- **Ethernet:** WAN + 4 LAN ports through the BCM53125 switch.
- **Device tree:** LEDs, reset/WPS buttons and USB port power follow the ASUS
  layout.
- **2.4 GHz Wi-Fi (b43):** the external PA enabled and closed-loop TX power
  control. 802.11g out of the box; **802.11n HT20** with the HT build
  (`HT=1`, see [802.11n](#24-ghz-80211n-ht20)): 32-35 down / 55-58 up
  Mbit/s on an iPhone, against 18/18 on 802.11g.

What doesn't:

- **5 GHz:** the 802.11ac core inside the SoC has no open Linux driver. It does
  work with Broadcom's own `wl` driver, which you build yourself from TP-Link's
  downloads (nothing closed is in this repo): see [wl/README.md](wl/README.md).
  That image also moves 2.4 GHz to `wl` (about twice b43's speed).
- **2.4 GHz extras:** sending two streams or STBC fails on this radio, and
  some clients (seen with an Apple Mac) lose A-MPDUs to the firmware. The HT
  build works around both (see below).

- **Forwarded traffic** with OpenWrt's kernel: the SoC's gmac corrupts some
  forwarded frames (UDP checksum errors, truncated packets; worst for Wi-Fi
  uploads). `build/build-kernel.sh` builds the kernel with the fix (64-byte TX
  DMA bursts); use it with `ZIMAGE=build/out-kernel/zImage build/make-image.sh`.

Status: tested on one hardware rev A unit. Use at your own risk.

## Hardware

| | |
|---|---|
| SoC | Broadcom BCM47189 (BCM53573 family), 1x Cortex-A7 900 MHz, 128 MB RAM |
| Flash | 16 MB SPI NOR: `boot` (CFE) 0x0–0x40000, `linux` (TRX) 0x40000–0xff0000, `nvram` 0xff0000– |
| Switch | BCM53125: wan = port 0, lan1–4 = ports 1–4, **CPU = port 8 ↔ gmac0** |
| 2.4 GHz | BCM43217 on PCIe: N-PHY rev 17, radio 2057 rev 14, **external PA** |
| 5 GHz | 802.11ac core integrated in the BCM47189 (no driver) |
| GPIOs | reset 5 (low), WPS 9 (low), power/WPS LED 10 (low), USB LED 15 (low), USB power 8, switch reset 2 |

## Quick start

Take a ready image from [Releases](../../releases). Each release has two:

- `…-asus_rt-ac1200g-plus-squashfs.trx`: **plain OpenWrt defaults.** root has no
  password, LAN is 192.168.1.1 with DHCP, Wi-Fi is off (SSID "OpenWrt"), WAN is
  DHCP client and closed. The fixed `b43.ko` is already included.
- `…-firstboot.trx`: also takes over the stock Asuswrt settings on first boot:
  2.4 GHz SSID/key, LAN IP, WAN MAC, and the root password from the ASUS admin
  password. It opens SSH/LuCI on WAN for private source IPs, which is meant for
  use behind another router. See `tools/rt-ac1200g-firstboot.sh`.

**Images up to v25.12.6-2 have no web interface** (LuCI was not included); use
SSH (`ssh root@192.168.1.1`), or build an image with the current scripts, which
add LuCI.

To flash, see [Flashing from stock firmware](#flashing-from-stock-firmware).
Then connect a cable to a LAN port and go to http://192.168.1.1. Set a root
password and enable Wi-Fi.

To build the same images yourself (Docker, about 10 min):

```sh
cd build && docker build --platform linux/amd64 -t owrt-sdk-env .
# SDK into build/dl (see "Building kmod-b43"), then:
./run-build.sh && ./make-image.sh          # FIRSTBOOT=1 ./make-image.sh for the variant
```

## How the image is made

OpenWrt's **Tenda AC9** image uses the same SoC, radio and switch. Its device
tree, however, makes DSA use switch port 5 as the CPU port. On this board only
port 8 is wired, so the Tenda image as-is boots with **no Ethernet at all**.

`tools/patch-trx-dtb.py` rewrites the device tree inside a Tenda AC9 TRX:

1. **Port fix:** switch port 5 is disabled, and port 8 gets a fixed link.
2. **Board fix:** the model name, LEDs, buttons, USB VBUS GPIO and `led-*`
   aliases are set for the ASUS. `compatible` stays `tenda,ac9`, so
   sysupgrade's board check keeps working.

The rootfs is untouched and the script verifies its own output. See
`docs/dtb-patch.diff` for exactly what changes.

```sh
pip install fdt            # plus liblzma >= 5.4 (macOS: brew install xz)
tools/patch-trx-dtb.py openwrt-25.12.x-bcm53xx-generic-tenda_ac9-squashfs.trx rt-ac1200g-plus.trx
```

`build/make-image.sh` does this on an ImageBuilder build that already has the
fixed `b43.ko`. By hand, you can also patch any Tenda AC9 image, for example
one from the [Firmware Selector](https://firmware-selector.openwrt.org/)
(target bcm53xx/generic) with extra packages. That image still has the stock
b43, so the 2.4 GHz radio stays weak until you install the fixed module.

### Flashing from stock firmware

The stock web UI refuses OpenWrt images. Instead:

1. Enable SSH in Asuswrt.
2. Copy the patched `.trx` to `/tmp` on the router.
3. Run `mtd-write2 /tmp/rt-ac1200g-plus.trx linux`, then reboot.

> **Never flash an unpatched image**: no plain Tenda AC9 image, and no LuCI
> "Attended Sysupgrade" (it fetches the stock Tenda image). Either one leaves
> you without Ethernet. Always patch first, then `sysupgrade`.
> Never write `boot` (CFE) or `nvram`.

**Recovery:** CFE has a rescue mode (tested: OpenWrt → stock and back).

1. Unplug power, hold reset, plug power in, keep holding ~10 s until the
   power LED blinks slowly, release.
2. The bootloader listens on **the LAN IP the stock firmware last had** (its
   nvram `lan_ipaddr`): 192.168.1.1 on a router that was never configured,
   otherwise whatever you set in Asuswrt. OpenWrt never changes it. Set a wired
   computer to an address in that subnet (e.g. 192.168.1.10/24), any LAN port.
3. `tftp` (binary) a *patched* `.trx` or the stock ASUS `.trx` to that IP, or
   upload it on the "CFE miniWeb Server" page at http://<that IP>/.

**Back to stock:** the rescue mode with the stock `.trx` from asus.com.
Firmware flashes never touch nvram, so the stock settings (admin password,
LAN IP, SSIDs) come back as they were before OpenWrt. From stock back to
OpenWrt: rescue mode again, or `mtd-write2 <file> linux` over stock SSH.

### Kernel-only update (keeps the overlay)

The DT lives in the kernel part of the TRX, so a DT change only touches the
flash blocks before the rootfs. Writing just those blocks keeps all installed
packages and config. A sysupgrade would wipe packages.

Caveat: on first boot, OpenWrt's `fixtrx` rewrites the on-flash TRX header so
that `len` covers only the kernel (0x30c400 here), with the CRC computed over
that range. A rebuilt header has to follow the same rule.

```sh
# compare the old and new image per 64 KiB block; build block 0 with the
# fixtrx-style header; then on the router, one call per changed block:
mtd -f -p <offset> write <block.bin> firmware
dd if=/dev/mtd1 bs=65536 count=<n> | sha256sum      # verify before reboot
```

## 2.4 GHz: b43 patches

Mainline b43 transmits about 26 dB too weakly on this board. Three things were
missing for BCM43217 with an external PA; all three were found by comparing
b43 with the vendor `wl.ko` and brcmsmac:

| Patch | Fix |
|---|---|
| `816` | 2 GHz EPA TX gain table for radio 2057 rev 14. b43 had none for N-PHY rev 7+. Extracted from the vendor `wl.ko` (`wlc_phy_get_epa_gaintbl_nphy`). |
| `818` | Enables the external PA/LNA control lines (PMU chipcontrol 1, bits 0x2000/0x2100), as `wlc_phy_init_nphy` does. Without this the external PA never turns on. Adds `nphy_txidx=N` (fixed gain index). |
| `819` | Routes the 2057 TSSI mux to the external power detector (`TX0/TX1_TX_SSI_MUX = 0x11`, `IQTEST_SEL_PU = 1`), as `wl` and brcmsmac do. This makes hardware TX power control work: it settles at the 20 dBm target instead of pinning maximum gain. |

Results (same spot, a MacBook's RSSI for the AP):

| Driver | RSSI | Notes |
|---|---|---|
| stock b43 | -73 dBm | 2.7 tries/packet |
| 816 + 818, fixed index 40 | -33…-44 dBm | 14/11 Mbit/s |
| 816 + 818 + 819, closed loop | +4 dB over idx 40 | 18/18 Mbit/s, 7 ms, 0.2 retries/packet, from farther away |

TX power set in LuCI/`iw` (`wireless.radio0.txpower`) is now honoured.

`patches/mac80211/brcm-debug/` and `brcm-tssi/` hold debug variants. `brcm-tssi`
adds `nphy_tssi_test=1`, which logs the power control loop state every 15 s.

## 2.4 GHz: 802.11n (HT20)

`patches/mac80211/brcm-ht/` is the b43 HT series from
[OpenWrt PR #25922](https://github.com/openwrt/openwrt/pull/25922) by Rosen
Penev (HT20, A-MPDU in both directions, two RX streams, STBC, NAPI, …),
rebased from backports 7.2 onto 25.12's 6.18.55 (only patch 813 needed a
manual change), plus this board's patches on top:

| Patch | |
|---|---|
| 880–882 | 816/818/819 above, renumbered to apply after the series |
| 883 | N-PHY rev 7+: one TX stream and no TX STBC by default. Each TX chain alone sends clean OFDM, but frames on both chains (STBC, MCS 8-15) mostly fail at the receiver. RX keeps MCS 0-15. Override: `ht_txchains`, `ht_stbc`. |
| 885 | Per-station A-MPDU fallback. The firmware (666.2 and 784.2 alike) ignores about a third of the block acks of an Apple 2x2 client, although they arrive on time (+1 µs median) and are well formed, so every MPDU of those A-MPDUs is sent again. When more than `ampdu_noba_pct` (35 %) of 64 A-MPDUs to a station get no block ack, send to it without A-MPDUs for 5 min. |

```sh
cd build && HT=1 ./run-build.sh      # -> build/out-ht/b43-stripped.ko
# on the router, after installing the module:
uci set wireless.radio0.htmode=HT20; uci commit wireless; wifi
```

Measured on the RT-AC1200G+ (WPA2, channel 1, 2-3 m):

| Client | 802.11g | HT20 |
|---|---|---|
| iPhone (1 stream, A-MPDU) | 18 / 18 Mbit/s | **32–35 / 55–58** (it sends to the AP at MCS 15) |
| Mac (2 streams, A-MPDU falls back) | 12 / 11 | 10–14 / **37–42** |

`patches/mac80211/brcm-ht-test/899-…` adds test switches (not in the
build): `txcore` (force the TX core mask), `ampdu_tx`, `ampdu_rts`,
`ampdu_log` / `ampdu_polls` (block ack verdict log), `nphy_pctl_log`.
`HT=1 PREPARE_ONLY=1 ./run-build.sh` only applies the patches (~1 min).

### Building `kmod-b43`

The build uses the OpenWrt SDK for the exact release, in Docker (`linux/amd64`;
it works on Apple Silicon too).

```sh
mkdir -p build/dl && cd build/dl
curl -LO https://downloads.openwrt.org/releases/25.12.6/targets/bcm53xx/generic/openwrt-sdk-25.12.6-bcm53xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64.tar.zst
cd .. && docker build --platform linux/amd64 -t owrt-sdk-env .
./run-build.sh            # -> build/out/b43-stripped.ko, kmod-b43-*.apk  (~6 min)
```

The `.ko` must match the router's kernel exactly (25.12.6: 6.12.112). Install
and test as follows:

- **Test without persisting:** `build/rt-test.sh build/out/b43-stripped.ko`
  (set `ROUTER=root@<ip>`). It loads the module from `/tmp`.
- **Persist:** copy the module over `/lib/modules/<kver>/b43.ko`. It lands in
  the overlay.
- **Roll back:** `rm /overlay/upper/lib/modules/<kver>/b43.ko; reboot`.

A `kmod-b43` upgrade or a sysupgrade replaces the module.

SDK gotcha: the SDK's `Config-build.in` forces `default m` on every release
package. `build-b43.sh` strips those defaults so that only mac80211/b43 get
built.

## Reverse-engineering helpers

`re/` holds small pyelftools/capstone scripts used on the vendor `wl.ko` (not
included; take it from the stock firmware's squashfs or the ASUS GPL tarball):

- symbol and `.data` reference lookup,
- ARM disassembly with relocations,
- immediate/word search.

`tools/wl-extract-epa-table.py` regenerates the 816 table from `wl.ko` and
checks itself against tables that b43/brcmsmac also carry.

`pip install pyelftools capstone`

## Not done / ideas

- **5 GHz:** needs an AC-PHY driver (b43's is a stub marked BROKEN). A USB
  adapter on an mt76 driver (e.g. MT7612U) is the realistic route. USB power
  is already enabled by the DT patch.
- **Upstreaming:** the b43 patches to linux-wireless, and a proper OpenWrt
  device definition.

## License

GPL-2.0-only (see `LICENSE`), matching the Linux kernel and OpenWrt code these
patches apply to.
