OpenWrt 25.12.6 for the ASUS RT-AC1200G+, with **802.11n (HT20) on 2.4 GHz**.

**Pre-release:** tested on one router: fresh install with defaults (LuCI, LAN,
2.4 GHz WPA2, USB, LEDs), sysupgrade with settings kept, reset button, CFE
rescue, back to stock and back to OpenWrt. See the README for flashing and
recovery, and verify the files against `sha256sums`. Only flash the `asus_rt-ac1200g-plus` images on an
RT-AC1200G+.

## What's new

- **LuCI is included.** Images up to v25.12.6-2 had no web interface at all.
- **Kernel with the bgmac fix** (`patches/kernel/950-…`, 64-byte TX DMA bursts):
  OpenWrt's kernel lets this SoC's gmac corrupt forwarded frames (UDP checksum
  errors, truncated packets; worst for uploads from Wi-Fi). Built by CI from the
  25.12.6 release config, so the release's kernel modules still load. The
  `zImage` is attached; source: OpenWrt v25.12.6 + that patch.
- **5 GHz is possible**, with Broadcom's `wl` driver built from your own TP-Link
  downloads (not in these images: it may not be redistributed). See
  `wl/README.md`. That image also moves 2.4 GHz to `wl`: 152/211 Mbit/s on
  5 GHz, 66/85 on 2.4 GHz (iPhone).
- 2.4 GHz runs as 802.11n HT20: the b43 HT series from OpenWrt PR #25922,
  rebased onto this release, plus this board's fixes (external PA, gain table,
  TSSI power control, one TX stream on N-PHY rev 7+, per-station A-MPDU
  fallback). `htmode` defaults to HT20 on a fresh install.
- Measured with an iPhone at 2-3 m: 32-35 Mbit/s down / 55-58 up, against
  18/18 on 802.11g.

## Known problems

- **Downloads to many clients gain little from 802.11n.** The Wi-Fi firmware
  ignores a large share of the block acks clients send for A-MPDUs: about a
  third for an Apple Mac, half or more for an Intel Windows laptop, ~20-40 % for
  an iPhone. The block acks arrive on time and are well formed (the `wl` driver
  on the same radio has no such problem), and newer firmware
  (784.2) does the same. The driver detects this per client and sends to that
  client without A-MPDUs (kernel log: `… A-MPDUs without block ack, sending
  without A-MPDUs for 300 s`). A Mac then downloads at about the 802.11g rate
  (10-14 Mbit/s here), uploads still gain (37-42 Mbit/s). Every 5 minutes the
  driver tries A-MPDUs again, which costs a few seconds; for a client that
  keeps failing the pause doubles each time, up to 40 minutes (patch 887). Tune or disable with
  the `b43` module parameter `ampdu_noba_pct` (default 35, 0 = never fall
  back).
- **macOS AWDL** (AirDrop, Handoff, Universal Control, AirPlay Receiver) takes
  the Mac's Wi-Fi off the channel several times a second and costs another
  third of its throughput to any 2.4 GHz access point. Turning those off
  helps; `sudo ifconfig awdl0 down` until the next reboot.
- **No two-stream TX, no STBC, no 40 MHz:** the router sends one stream (MCS
  0-7) and receives two (MCS 0-15). Sending on both chains (`ht_txchains=2`)
  now connects at MCS 13-15, but more frames fail and a Mac downloaded less
  (3 vs 8 Mbit/s), so it stays off by default (`ht_stbc` to experiment too).
- 5 GHz: no open driver (see `wl/README.md`).

## Rolling back to 802.11g

`uci set wireless.radio0.htmode=NOHT; uci commit wireless; wifi`
