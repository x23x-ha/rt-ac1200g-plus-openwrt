#!/bin/sh
# Add the wl radios to fw4's flowtable so flows to Wi-Fi clients get direct
# xmit info (and the fast path can hand them straight to the driver). The
# devices only exist once wl.ko is loaded, so this cannot
# live in the static 20-fastpath.nft: it runs from a fw4 script include
# (after every firewall reload) and from hotplug when a device appears.
# A wl interface is one the stock `wl` tool can talk to.
WL="env LD_LIBRARY_PATH=/usr/lib/wl/lib /usr/lib/wl/bin/wl"
cur=$(nft list flowtable inet fw4 ft 2>/dev/null) || exit 0
for p in /sys/class/net/eth*; do
	dev=${p##*/}
	$WL -i "$dev" bands >/dev/null 2>&1 || continue
	echo "$cur" | grep -q "\"$dev\"" && continue
	nft add flowtable inet fw4 ft "{ hook ingress priority filter; devices = { $dev }; }"
done
