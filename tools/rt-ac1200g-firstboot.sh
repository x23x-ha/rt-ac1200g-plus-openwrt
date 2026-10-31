# RT-AC1200G+ first boot (runs once, on OpenWrt's first start)
# Reuses what the stock ASUS firmware left in its nvram, which OpenWrt does not erase:
# 2.4 GHz Wi-Fi name and security, LAN IP, WAN MAC and last WAN IP.

# Root password for SSH and LuCI. If left empty, the ASUS admin password from
# the stock nvram (http_passwd) is used. If there is none either, root keeps
# OpenWrt's default (no password) and SSH/LuCI are NOT opened on WAN.
ROOT_PW=''

# Optional Wi-Fi overrides. Leave empty to copy the ASUS 2.4 GHz settings
# (an open network stays open).
SSID=''
KEY=''

nv() { nvram get "$1" 2>/dev/null; }

[ -n "$SSID" ] || SSID="$(nv wl0_ssid)"
[ -n "$SSID" ] || SSID='OpenWrt-AC1200G'
if [ -z "$KEY" ]; then
	case "$(nv wl0_auth_mode_x)" in
		psk|psk2|pskpsk2|sae|psk2sae) KEY="$(nv wl0_wpa_psk)" ;;
	esac
fi
if [ ${#KEY} -ge 8 ]; then ENC='psk2'; else ENC='none'; KEY=''; fi
[ -n "$ROOT_PW" ] || ROOT_PW="$(nv http_passwd)"
[ -n "$ROOT_PW" ] && printf '%s\n%s\n' "$ROOT_PW" "$ROOT_PW" | passwd root >/dev/null 2>&1

uci set system.@system[0].hostname='RT-AC1200G'
uci commit system

# LAN: keep the ASUS subnet so it can't clash with the upstream router's
LANIP="$(nv lan_ipaddr)"
LANMASK="$(nv lan_netmask)"
case "$LANIP" in ''|0.0.0.0) LANIP='192.168.2.1'; LANMASK='255.255.255.0' ;; esac
[ -n "$LANMASK" ] || LANMASK='255.255.255.0'
uci -q delete network.lan.ipaddr
uci set network.lan.ipaddr="$LANIP"
uci set network.lan.netmask="$LANMASK"

# WAN: same MAC as the ASUS used, and ask the upstream router for the same IP
WANMAC="$(nv wan0_hwaddr)"
WANIP="$(nv wan0_ipaddr)"
if echo "$WANMAC" | grep -qiE '^([0-9a-f]{2}:){5}[0-9a-f]{2}$'; then
	WANDEV="$(uci -q show network | sed -n "s/^network\.\([^.]*\)\.name='wan'$/\1/p" | head -n1)"
	if [ -z "$WANDEV" ]; then
		WANDEV="$(uci add network device)"
		uci set network.$WANDEV.name='wan'
	fi
	uci set network.$WANDEV.macaddr="$WANMAC"
fi
case "$WANIP" in ''|0.0.0.0) ;; *) uci set network.wan.reqaddress="$WANIP" ;; esac
uci commit network

# Fallback access when the router sits behind another router: SSH and LuCI
# from the upstream LAN (WAN side), private source addresses only, and only
# with a root password set. Named section, so re-runs don't add duplicates.
# Delete it (uci delete firewall.wanadmin) before WAN faces the internet.
if [ -n "$ROOT_PW" ]; then
	R=wanadmin
	uci -q delete firewall.$R
	uci set firewall.$R=rule
	uci set firewall.$R.name='Allow-SSH-LuCI-WAN'
	uci set firewall.$R.src='wan'
	uci set firewall.$R.proto='tcp'
	uci set firewall.$R.dest_port='22 80 443'
	for n in 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16; do
		uci add_list firewall.$R.src_ip="$n"
	done
	uci set firewall.$R.target='ACCEPT'
	uci commit firewall
fi

# Wi-Fi: the radio may not be detected yet at this point in boot,
# so a one-shot helper turns it on from rc.local at the end of boot.
printf '%s\n%s\n%s\n' "$SSID" "$ENC" "$KEY" > /etc/wifi-firstboot.conf
chmod 600 /etc/wifi-firstboot.conf

cat > /etc/wifi-firstboot.sh <<'EOF'
#!/bin/sh
[ -f /etc/wifi-firstboot.conf ] || exit 0
{ IFS= read -r SSID; IFS= read -r ENC; IFS= read -r KEY; } < /etc/wifi-firstboot.conf

i=0
while [ $i -lt 30 ] && [ -z "$(ls /sys/class/ieee80211 2>/dev/null)" ]; do
	sleep 1; i=$((i+1))
done

wifi config
radios="$(uci -q show wireless | sed -n "s/^wireless\.\([^.=]*\)=wifi-device$/\1/p")"
if [ -z "$radios" ]; then
	logger -t wifi-firstboot "no Wi-Fi radio detected, will retry next boot"
	exit 0
fi

for r in $radios; do
	uci set wireless.$r.disabled='0'
	[ "$(uci -q get wireless.$r.band)" = "2g" ] && uci set wireless.$r.channel='6'
done
for s in $(uci -q show wireless | sed -n "s/^wireless\.\([^.=]*\)=wifi-iface$/\1/p"); do
	uci set wireless.$s.mode='ap'
	uci set wireless.$s.network='lan'
	uci set wireless.$s.ssid="$SSID"
	uci set wireless.$s.encryption="$ENC"
	if [ "$ENC" = none ]; then
		uci -q delete wireless.$s.key
	else
		uci set wireless.$s.key="$KEY"
	fi
	uci -q delete wireless.$s.disabled
done
uci commit wireless
wifi reload
logger -t wifi-firstboot "Wi-Fi enabled: $SSID"

rm -f /etc/wifi-firstboot.conf
sed -i '/wifi-firstboot/d' /etc/rc.local
EOF
chmod +x /etc/wifi-firstboot.sh

cat > /etc/rc.local <<'EOF'
# Put your custom commands here that should be executed once
# the system init finished. By default this file does nothing.
/etc/wifi-firstboot.sh
exit 0
EOF

exit 0
