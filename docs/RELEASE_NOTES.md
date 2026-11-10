OpenWrt 25.12.6 for the ASUS RT-AC1200G+, with **802.11n (HT20) on 2.4 GHz**.

**Pre-release:** tested on one router: fresh install with defaults (LuCI, LAN,
2.4 GHz WPA2, USB, LEDs), sysupgrade with settings kept, reset button, CFE
rescue, back to stock and back to OpenWrt. See the README for flashing and
recovery, and verify the files against `sha256sums`. Only flash the `asus_rt-ac1200g-plus` images on an
RT-AC1200G+.

## What's new in v25.12.6-6

- **2.4 GHz: second receive chain fixed (patch 891).** b43 set a chip control
  bit that, on the BCM43217, cuts the second chain's reception by about 30 dB.
  The ASUS firmware leaves it clear; 891 clears it. Per-chain signal went from
  -49/-80 to -44/-48 dBm. iPhone at the same spot (down / up Mbit/s):
  71/113, 67/96, 69/91, 67/49, 53/70, against 61/47, 45/60, 56/62 before.
  Upload now matches or beats the Broadcom `wl` driver (73/80 at best).
- **Per-chain RSSI (patch 890):** `iw dev phy0-ap0 station dump | grep signal`
  shows both chains, e.g. `-44 [-44, -48]`.
- `wl/` (5 GHz, build it yourself): 2.4 GHz now stays on b43 by default;
  `wireless.radio0.use_wl=1` moves it to `wl` (replaces `keep_b43`).
- README: new section "NAT fast path".
- Tested on the author's router with the same b43 module (63c5f917), settings
  kept. The release images themselves were not fresh-installed. Kernel and fast
  path unchanged from v25.12.6-5.

## v25.12.6-5

- **NAT fast path** (`fastpath.ko`, from Vygis1's Archer C1200 v2 port): wired
  NAT about doubles, 758-819 Mbit/s down against 378-411 with flow offloading
  alone. It only works with **Software flow offloading** on (LuCI: Network ->
  Firewall), which a fresh install has off. See the README section
  "NAT fast path".
- The kernel now carries the fast-path hook (patch 323, which also includes the
  bgmac 64-byte TX burst fix of the earlier releases) and 324 (RX headroom).
  `zImage-6.12.112-fastpath` and `fastpath-6.12.112.ko` are attached.
- Tested on the author's router with the same kernel and module (flashed, 30 min
  load test); the release images themselves were not fresh-installed.
- 2.4 GHz driver and firmware unchanged from v25.12.6-4.

## v25.12.6-4

- **New 2.4 GHz driver patches: two streams and A-MPDU now work.** The b43 set
  is now OpenWrt PR #25922 at 1e3c416ee5, which includes Ville Takio's
  BCM43217 fixes (2.4 GHz external PA gain table, RX gain control, external
  PAs enabled after a cold power-on, TSSI routing, RX and TX I/Q calibration),
  plus his 880/881 (64 KiB A-MPDU receive, 64-frame block ack window). This
  board's own workarounds (one TX stream, per-station A-MPDU fallback) are
  gone: the problems they hid no longer show up.
- **Firmware 784.2** (broadcom-wl 6.30.163.46) for the 2.4 GHz chip, as in the PR.
- Measured on the author's router after a cold power-on, channel 1 HT20, WPA2,
  clients a few metres away, down / up Mbit/s, best of three runs:

  | Client | v25.12.6-3 | v25.12.6-4 | `wl` driver |
  |---|---|---|---|
  | iPhone (2x2) | 32-35 / 55-58 | 63 / 49 | 66 / 85 |
  | Mac (2x2) | 10-14 / 37-42 | 58 / 51 | not measured |
  | Windows laptop, Intel (1 stream) | ~20 / 24 | 40 / 37 | 49-52 / 36-43 |

  The router now sends MCS 15 to two-stream clients. The module was tested
  on the running router, not yet by a fresh install of these images.

## Earlier changes

- **LuCI is included.** Images up to v25.12.6-2 had no web interface at all.
- **Kernel with the bgmac fix** (`patches/kernel/950-…`, 64-byte TX DMA bursts):
  OpenWrt's kernel lets this SoC's gmac corrupt forwarded frames (UDP checksum
  errors, truncated packets; worst for uploads from Wi-Fi). Built by CI from the
  25.12.6 release config, so the release's kernel modules still load. The
  `zImage` is attached; source: OpenWrt v25.12.6 + that patch.
- **5 GHz is possible**, with Broadcom's `wl` driver built from your own TP-Link
  downloads (not in these images: it may not be redistributed). See
  `wl/README.md`. That image also moves 2.4 GHz to `wl`.

## Known problems

- **iPhone upload** is still well below `wl` (49 vs 85 Mbit/s), and the
  Intel laptop's download about 20 % below.
- The first speed test after a client connects is often slow (15-26 Mbit/s);
  the next runs are not.
- **macOS AWDL** (AirDrop, Handoff, Universal Control, AirPlay Receiver) takes
  the Mac's Wi-Fi off the channel several times a second and costs a share of
  its throughput to any 2.4 GHz access point. `sudo ifconfig awdl0 down` until
  the next reboot.
- No 40 MHz on 2.4 GHz.
- 5 GHz: no open driver (see `wl/README.md`).

## Rolling back to 802.11g

`uci set wireless.radio0.htmode=NOHT; uci commit wireless; wifi`
