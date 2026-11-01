OpenWrt 25.12.6 for the ASUS RT-AC1200G+, with **802.11n (HT20) on 2.4 GHz**.

**Pre-release:** tested on one router. The images have not yet been tested
from a fresh install; see the README for flashing and recovery, and verify the
files against `sha256sums`. Only flash the `asus_rt-ac1200g-plus` images on an
RT-AC1200G+.

## What's new

- 2.4 GHz runs as 802.11n HT20: the b43 HT series from OpenWrt PR #25922,
  rebased onto this release, plus this board's fixes (external PA, gain table,
  TSSI power control, one TX stream on N-PHY rev 7+, per-station A-MPDU
  fallback). `htmode` defaults to HT20 on a fresh install.
- Measured with an iPhone at 2-3 m: 32-35 Mbit/s down / 55-58 up, against
  18/18 on 802.11g.

## Known problems

- **Downloads to some Apple Macs gain nothing from 802.11n.** The Wi-Fi
  firmware ignores about a third of the block acks a Mac (Apple 2x2 chip) sends
  for A-MPDUs. They arrive on time and are well formed, and newer firmware
  (784.2) does the same. The driver detects this per client and sends to that
  client without A-MPDUs (kernel log: `… A-MPDUs without block ack, sending
  without A-MPDUs for 300 s`). The Mac then downloads at about the 802.11g rate
  (10-14 Mbit/s here), uploads still gain (37-42 Mbit/s). Every 5 minutes the
  driver tries A-MPDUs again, which costs a few seconds. Tune or disable with
  the `b43` module parameter `ampdu_noba_pct` (default 35, 0 = never fall
  back).
- **macOS AWDL** (AirDrop, Handoff, Universal Control, AirPlay Receiver) takes
  the Mac's Wi-Fi off the channel several times a second and costs another
  third of its throughput to any 2.4 GHz access point. Turning those off
  helps; `sudo ifconfig awdl0 down` until the next reboot.
- **No two-stream TX, no STBC, no 40 MHz:** the router sends one stream (MCS
  0-7) and receives two (MCS 0-15). Sending on both chains fails on this radio
  and is off by default (`ht_txchains`, `ht_stbc` to experiment).
- 5 GHz has no driver.

## Rolling back to 802.11g

`uci set wireless.radio0.htmode=NOHT; uci commit wireless; wifi`
