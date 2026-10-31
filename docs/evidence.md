# Raw evidence collected on 2026-10-31 (first session)

Kept verbatim (trimmed; MAC addresses redacted) so the conclusions in the README can be re-checked.

## Stock firmware (Asuswrt 3.0.0.4.382_52272)

```
# cat /proc/cpuinfo   -> ARMv7 rev 5, CPU part 0xc07 (Cortex-A7), BogoMIPS 1795.68, "Northstar Prototype"
# cat /proc/mtd
mtd0: 00040000 00010000 "boot"     # CFE  0x000000-0x040000
mtd1: 00fb0000 00010000 "linux"    # TRX  0x040000-0xff0000
mtd2: 00d7d270 00010000 "rootfs"   # inside linux
mtd3: 00090000 00010000 "jffs2"    # inside linux (end)
mtd4: 00010000 00010000 "nvram"    # 0xff0000-0x1000000
# nvram get productid -> RT-AC1200G+
# nvram (GPIO-related; ASUS adds 4096 = active low, 255 = unused)
btn_rst_gpio=4101  reset_gpio=5      -> reset button GPIO 5, active low
btn_wps_gpio=4105  gpio9=wps_button  -> WPS button GPIO 9, active low
led_pwr_gpio=4106  led_wps_gpio=4106 -> power/WPS LED GPIO 10, active low
led_usb_gpio=4111                    -> USB LED GPIO 15, active low
gpio8=usbport1                       -> USB port power GPIO 8 (active high)
gpio2=robo_reset                     -> switch (BCM53125) reset GPIO 2
wanports=0  lanports=1 2 3 4  vlan1ports=1 2 3 4 8*  vlan2ports=0 8u   -> CPU port 8
et0macaddr=04:D4:C4:xx:xx:08  0:macaddr=04:D4:C4:xx:xx:08  sb/1/macaddr=04:D4:C4:xx:xx:0C
# flash tools present: mtd-write2 (usage: mtd-write2 <file> <partition>), mtd-erase2, mtd-unlock
# web UI firmware upload of an ASUS-tagged OpenWrt TRX: silently refused (no log, firmware_check unset)
```

## OpenWrt boot with the *unpatched* Tenda AC9 DTB (DSA CPU = port 5 / eth1)

```
bgmac_bcma bcma0:5: Found PHY addr: 30 (NOREGS)          # gmac0 -> eth0
bgmac_bcma bcma0:10: Found PHY addr: 0                   # gmac1 -> eth1
bgmac_bcma bcma0:10: Invalid MAC addr: 00:00:00:00:00:00 # nvram has no et1macaddr
... CPU port 8 lacks the required "phy-handle", "fixed-link" or "managed" properties
4: wan@eth1: ... state UP
eth0 rx=0 tx=0 | eth1 rx=0 tx=50 | wan rx=0 tx=34        # DHCP discovers leave, nothing returns
```

## OpenWrt with the patched DTB (port 5 disabled, port 8 fixed-link)

```
cat .../switch@1e/ports/port@5/status -> disabled
4: wan@eth0: ... state UP
eth0 rx=2147 tx=2823 | eth1 rx=0 tx=0 | wan rx=2147 tx=2807
udhcpc: lease of 192.168.x.51 obtained from 192.168.x.1, lease time 7200
lan1 carrier=1 (client), DHCP 192.168.2.169 OK
```


## 2.4 GHz radio (b43)

```
b43-phy0: Broadcom 43217 WLAN found (core revision 30)
b43-phy0: Found PHY: Analog 9, Type 4 (N), Revision 17
b43-phy0: Found Radio: Manuf 0x17F, ID 0x2057, Revision 14, Version 1
b43-phy0: Loading firmware version 666.2 (2011-02-23 01:15:07)
b43-phy0 ERROR: No 2GHz EPA gain table available for this device   (logged twice)
iw: phy0-ap0 type AP, channel 6 (2437 MHz), width: 20 MHz (no HT), txpower 20.00 dBm
hostapd: AP-ENABLED, no crashes; clients drop with "disassociated/deauthenticated due to inactivity"
```

RSSI at a MacBook, same spot, Option-click Wi-Fi menu:
- ASUS (OpenWrt, b43): RSSI -73 dBm, noise -99 dBm
- upstream AP:        RSSI -47 dBm, noise -88 dBm

b43 source (mainline, tables_nphy.c, b43_nphy_get_tx_gain_table): the 2 GHz EPA
branch only covers PHY rev 3-6; rev 7+ hits `default:` -> error + NULL.
IPA table for this exact radio exists: b43_ntab_tx_gain_ipa_2057_rev14_2g
(first words 0x50df002e 0x50cf002d 0x50bf002c 0x50b7002b ...), as does
b43_ntab_rf_pwr_offset_2057_rev14_2g. phy_n.c has 16 `b43_nphy_ipa(dev)` branches.

## 5 GHz

b43 phy_ac.c is an 88-line stub; Kconfig: `B43_PHY_AC ... depends on BROKEN`,
"Say N, this is BROKEN and crashes driver" (lists only PCI BCM4352/BCM4360).
The 5 GHz radio here is the 802.11ac core integrated in the BCM47189 SoC.
