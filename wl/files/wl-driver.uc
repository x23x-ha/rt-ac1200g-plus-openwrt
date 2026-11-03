#!/usr/bin/ucode
// netifd wireless driver for the Broadcom wl blob - both radios: the 5 GHz AC
// core on the SoC backplane and the 2.4 GHz BCM43217 on PCIe.
//
// wl is a fullmac vendor driver with its own ioctl API, not cfg80211, so none of
// the hostapd/nl80211 machinery applies: this shells out to the stock `wl` tool
// and lets `nas` do the WPA2 handshake. One AP BSS per radio - no multi-SSID,
// no client/mesh modes.
//
// netifd runs these scripts as ucode (see mac80211.sh), and importing
// wifi.common is what establishes global.ubus for netifd.notify().
'use strict';

import { log } from 'wifi.common';
import * as netifd from 'wifi.netifd';
import * as fs from 'fs';

global.radio = ARGV[2];

// RT-AC1200G+: module and stock tools ship read-only in the image (squashfs)
const WLDIR = '/usr/lib/wl';
const ENV = `LD_LIBRARY_PATH=${WLDIR}/lib`;
// one file per radio that is up, holding its nas arguments (see restart_auth)
const STATE = '/var/run/wl';

// blobmsg types, as netifd expects them in the option dump
const T_ARRAY = 1, T_STRING = 3, T_INT32 = 5, T_BOOL = 7;

// Set per invocation from the radio's band: the interface and the unit (its
// nvram prefix). wl.ko attaches the SoC radio at insmod and the PCIe one
// only when setup binds it, so the 5 GHz radio is always unit 0 (wl0_) and
// the 2.4 GHz one unit 1 (wl1_).
let IFACE = null, UNIT = 0;

function shq(s) {
	s = `${s ?? ''}`;
	return "'" + replace(s, "'", "'\\''") + "'";
}

function sh(cmd) {
	return system(`${cmd} >/dev/null 2>&1`);
}

function sh_out(cmd) {
	let p = fs.popen(cmd + ' 2>/dev/null', 'r');
	if (!p)
		return '';
	let out = p.read('all') ?? '';
	p.close();
	return trim(out);
}

function wl_on(ifname, ...args) {
	return sh(`${ENV} ${WLDIR}/bin/wl -i ${ifname} ` + join(' ', args));
}

function wl_cmd(...args) {
	return wl_on(IFACE, ...args);
}

function nvset(k, v) {
	return sh(`${ENV} ${WLDIR}/bin/nvram set ${shq(k + '=' + v)}`);
}

function nvunit(k, v) {
	return nvset(`wl${UNIT}_${k}`, v);
}

// The /dev/nvram char device has a dynamic misc minor; libnvram needs the node.
function mknvram() {
	/* The misc minor is dynamic, so a node left over from a previous boot
	 * can point at the wrong device. Always recreate it. */
	let misc = fs.readfile('/proc/misc') ?? '';
	let m = match(misc, /([0-9]+) nvram/);
	if (!m)
		return false;
	sh('rm -f /dev/nvram');
	return sh(`mknod /dev/nvram c 10 ${m[1]}`) == 0;
}

// The wl interface on a band: `wl bands` answers "a" for 5 GHz, "b" for 2.4.
function find_iface(band) {
	let want = (band == '2g') ? 'b' : 'a';
	for (let e in fs.lsdir('/sys/class/net') ?? []) {
		if (!match(e, /^eth[0-9]+$/))
			continue;
		let b = sh_out(`${ENV} ${WLDIR}/bin/wl -i ${e} bands`);
		if (b && index(b, want) >= 0)
			return e;
	}
	return null;
}

/* The radio's band, from its config or - for teardown, which may come
 * without one - from UCI by radio name. */
function radio_band(dcfg) {
	return dcfg?.band || sh_out(`uci -q get wireless.${global.radio}.band`) || '5g';
}

function parse_cfg() {
	try {
		return json(ARGV[3] || '{}') ?? {};
	} catch (e) {
		return {};
	}
}

function wait_iface(band) {
	for (let i = 0; i < 20; i++) {
		let e = find_iface(band);
		if (e)
			return e;
		sleep(500);
	}
	return null;
}

function load_module() {
	let mods = fs.readfile('/proc/modules') ?? '';
	if (match(mods, /^wl /) || index(mods, '\nwl ') >= 0)
		return true;
	return sh(`insmod ${WLDIR}/wl.ko`) == 0;
}

/* The BCM43217 is a PCIe device that the bcma host driver claims at boot
 * (and b43 its D11 core). Hand it to wl.ko's PCI driver instead. */
function bind_pci_radio() {
	let pci = null;
	for (let d in fs.lsdir('/sys/bus/pci/devices') ?? []) {
		let p = `/sys/bus/pci/devices/${d}`;
		if (trim(fs.readfile(`${p}/vendor`) ?? '') == '0x14e4' &&
		    trim(fs.readfile(`${p}/class`) ?? '') == '0x028000')
			pci = d;
	}
	if (!pci)
		return false;
	if (fs.stat(`/sys/bus/pci/drivers/wl-pci/${pci}`))
		return true;
	if (fs.stat(`/sys/bus/pci/drivers/bcma-pci-bridge/${pci}`))
		fs.writefile('/sys/bus/pci/drivers/bcma-pci-bridge/unbind', pci);
	fs.writefile('/sys/bus/pci/drivers/wl-pci/bind', pci);
	return !!fs.stat(`/sys/bus/pci/drivers/wl-pci/${pci}`);
}

function dump_options() {
	printf('%J\n', {
		name: 'broadcom',
		device: [
			[ 'channel', T_STRING ], [ 'hwmode', T_STRING ], [ 'band', T_STRING ],
			[ 'htmode', T_STRING ], [ 'noscan', T_STRING ], [ 'country', T_STRING ],
			[ 'wl_country', T_STRING ],
			[ 'txpower', T_INT32 ], [ 'disabled', T_BOOL ],
		],
		iface: [
			[ 'mode', T_STRING ], [ 'ssid', T_STRING ], [ 'encryption', T_STRING ],
			[ 'key:wpakey', T_STRING ], [ 'bridge_isolate', T_BOOL ],
			[ 'tags', T_ARRAY ], [ 'isolate', T_BOOL ], [ 'hidden', T_BOOL ],
		],
		// netifd spins if these come back empty, so declare the same
		// vlan/station options mac80211 does even though we ignore them.
		vlan: [
			[ 'name', T_STRING ], [ 'vid', T_STRING ], [ 'iface', T_ARRAY ],
			[ 'network', T_ARRAY ], [ 'network_vlan', T_ARRAY ],
			[ 'bridge_isolate', T_BOOL ], [ 'isolate', T_BOOL ],
			[ 'multicast_to_unicast', T_BOOL ], [ 'proxy_arp', T_BOOL ],
		],
		station: [
			[ 'iface', T_ARRAY ], [ 'mac', T_ARRAY ],
			[ 'key', T_STRING ], [ 'vid', T_STRING ],
		],
	});
	return 0;
}

/*
 * eapd and nas serve every radio at once, so whenever one comes or goes
 * both are restarted for the current set (the files in STATE). eapd walks
 * lan_ifnames and matches each interface's live MAC against wlX_hwaddr; it
 * owns the raw sockets for the Broadcom event channel (0x886c) and preauth
 * and relays EAPOL to nas over loopback UDP, so it starts first. nas runs in
 * its stock mode first - no arguments, everything from nvram (wlX_akm,
 * wlX_crypto, wlX_wpa_psk, ...) - which serves all radios from one process;
 * if that does not stay up, one argument-mode nas per radio. They are not
 * registered with netifd: a radio's setup restarting them would otherwise
 * make netifd tear down the other radio.
 */
function restart_auth() {
	/* a background start from an earlier setup (see below) must not race this one */
	sh('[ -f /var/run/wl-auth-retry.pid ] && kill $(cat /var/run/wl-auth-retry.pid); rm -f /var/run/wl-auth-retry.pid');
	sh('killall nas eapd');
	let ifs = [], wpa = [];
	for (let f in fs.lsdir(STATE) ?? []) {
		push(ifs, f);
		let args = trim(fs.readfile(`${STATE}/${f}`) ?? '');
		if (length(args))
			push(wpa, args);
	}
	nvset('lan_ifname', 'br-lan');
	nvset('lan_ifnames', join(' ', ifs));
	nvset('wl_ifnames', join(' ', ifs));
	if (!length(wpa))
		return;
	sleep(500);
	sh(`${ENV} ${WLDIR}/bin/eapd`);
	sleep(500);
	sh(`${ENV} ${WLDIR}/bin/nas &`);
	sleep(1500);
	if (sh_out('pidof nas') == '') {
		log('wl: nas did not stay up from nvram, starting one per radio');
		for (let a in wpa)
			sh(`${ENV} ${WLDIR}/bin/nas ${a} &`);
		sleep(1000);
	}
	if (sh_out('pidof eapd') != '' && sh_out('pidof nas') != '')
		return;
	/* At boot the FIRST radio's start leaves eapd down (nas up), every boot
	 * since 2026-11-03; at runtime it never fails, and 3 quick retries did
	 * not help. Cause unknown: log eapd's own output, then keep starting whatever is
	 * missing in the background for 60 s (never kills, never a second nas) (the second radio's setup usually fixes it
	 * anyway, but not when 2.4 GHz is the only radio). */
	let why = sh_out(`${ENV} ${WLDIR}/bin/eapd >/tmp/wl-eapd.out 2>&1; echo rc=$?; cat /tmp/wl-eapd.out`);
	log(`wl: eapd/nas not up (eapd ${sh_out('pidof eapd') == '' ? 'down' : 'up'}, nas ${sh_out('pidof nas') == '' ? 'down' : 'up'}); eapd says: ${replace(why, /\n/g, ' | ')}`);
	let L = `${ENV} ${WLDIR}/bin`;
	sh(`setsid sh -c 'echo $$ > /var/run/wl-auth-retry.pid; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do sleep 5; ` +
	   `if pidof eapd >/dev/null && pidof nas >/dev/null; then rm -f /var/run/wl-auth-retry.pid; logger -t wl-auth "eapd/nas up after $((i*5)) s"; exit 0; fi; ` +
	   `pidof eapd >/dev/null || ${L}/eapd; sleep 1; pidof nas >/dev/null || ${L}/nas & ` +
	   `done; rm -f /var/run/wl-auth-retry.pid; logger -t wl-auth "eapd/nas still down after 60 s - WPA2 will not work"' </dev/null >/dev/null 2>&1 &`);
}

function setup_vif(section, icfg, dcfg) {
	let ssid = icfg.ssid ?? 'OpenWrt';
	let key = icfg.key ?? '';
	let enc = icfg.encryption ?? 'none';
	/* RT-AC1200G+: the blob's locale comes ONLY from the radio's own
	 * 'wl_country' option, default the board's ASUS code E0/943 (80 MHz on
	 * 36-64, 100-112; verified 2026-11-02). The shared 'country' option is
	 * ignored: wifi-scripts apply one country to all radios (radio0's hostapd
	 * and the kernel), and codes valid there can be empty in the blob
	 * (RU/0 has no channels at all). */
	let country = dcfg.wl_country ?? 'E0/943';
	let g2 = (dcfg.band == '2g');
	let chan = dcfg.channel;
	if (!chan || chan == 'auto')
		chan = g2 ? 6 : 36;

	log(`wl: ${IFACE} (wl${UNIT}) setup ssid='${ssid}' enc='${enc}' key=${length(key) ? 'set' : 'unset'} ch=${chan} country=${country}`);

	/* eapd matches each lan_ifnames interface's live MAC against nvram,
	 * so wlX_hwaddr must be right or it finds nothing and exits 255. */
	let mac = uc(trim(fs.readfile(`/sys/class/net/${IFACE}/address`) ?? ''));
	if (UNIT == 0)
		nvset('wl_hwaddr', mac);
	nvunit('hwaddr', mac);           nvunit('ifname', IFACE);
	nvunit('unit', `${UNIT}`);       nvunit('mode', 'ap');
	nvunit('infra', '1');            nvunit('radio', '1');
	nvunit('bss_enabled', '1');      nvunit('ssid', ssid);
	nvunit('wpa_psk', key);          nvunit('wpa_gtk_rekey', '3600');
	nvunit('auth_mode', 'none');     nvunit('auth', '0');
	nvunit('wep', 'disabled');       nvunit('country_code', split(country, '/')[0]);

	wl_cmd('down');
	wl_cmd('mpc', '0');
	/* The blob's per-country tables (NL/0, DE/0, E0/0) carry no 80 MHz
	 * chanspecs, so 36/80 is a "Bad Channel". Broadcom's EU-wide EU/13 has
	 * them (36-48, 52-64, 100-112), so European countries use that. */
	let eu = [ 'AT','BE','BG','CH','CY','CZ','DE','DK','EE','ES','FI','FR','GB','GR','HR','HU',
		'IE','IS','IT','LI','LT','LU','LV','MT','NL','NO','PL','PT','RO','SE','SI','SK' ];
	wl_cmd('country', (country in eu) ? 'EU/13' : country);
	wl_cmd('ap', '1');
	wl_cmd('closed', icfg.hidden ? '1' : '0');

	/* HT/VHT and channel width. NOHT/LEGACY = 802.11a/g, HTxx = 802.11n,
	 * VHTxx = 802.11ac (5 GHz only); the width goes into the chanspec
	 * ("36/80"). Empty defaults to VHT80 on 5 GHz, HT20 on 2.4. */
	let ht = dcfg.htmode ?? '';
	if (ht == '')
		ht = g2 ? 'HT20' : 'VHT80';
	let bw = match(ht, /(20|40|80)$/);
	bw = bw ? bw[1] : '20';
	let spec = `${chan}`;
	if (ht == 'NOHT' || ht == 'LEGACY') {
		wl_cmd('nmode', '0');
		if (!g2)
			wl_cmd('vhtmode', '0');
	} else {
		wl_cmd('nmode', '1');
		if (!g2)
			wl_cmd('vhtmode', substr(ht, 0, 3) == 'VHT' ? '1' : '0');
		/* wl names a 2.4 GHz 40 MHz channel by its control channel plus the
		 * side it sits on: "1l" = control 1, the 20 MHz above it added. The
		 * 2.4 GHz bandwidth cap is 20 MHz only until bw_cap allows 40 (3). */
		if (g2)
			wl_cmd('bw_cap', `2g ${bw == '40' ? 3 : 1}`);
		if (bw != '20')
			spec = g2 ? `${chan}${+chan <= 7 ? 'l' : 'u'}` : `${chan}/${bw}`;
	}
	log(`wl: ${IFACE} htmode=${ht} chanspec=${spec}`);
	/* Packet chaining must stay on: with pktc 0 the A-MPDU transmit path
	 * stalls (HT/VHT clients never get WPA2 message 3). Chaining needs
	 * pub->brc_hot, which the shim allocates (compat.c k26_brc_hot_init). */
	wl_cmd('pktc', '1');
	/* TXOP frame bursting, as stock: same throughput, a little less CPU */
	wl_cmd('frameburst', '1');
	wl_cmd('ap_isolate', icfg.isolate ? '1' : '0');

	let nas_args = '';
	if (match(enc, /psk2|wpa2|ccmp/)) {
		nvunit('akm', 'psk2');
		nvunit('crypto', 'aes');
		wl_cmd('wsec', '4');        // AES-CCMP
		wl_cmd('wpa_auth', '128');  // WPA2-PSK
		wl_cmd('eap', '1');
		/* argument-mode fallback - getopt "B:g:h:i:I:k:r:K:l:m:N:p:s:t:v:w:d:ADS",
		 * -i first (until an interface is accepted every other option
		 * aborts with "Command line parsing error"); -m is the wpa_auth
		 * bitmask, -w the wsec one. */
		nas_args = sprintf('-i %s -A -m 128 -w 4 -s %s -k %s -g 3600',
			IFACE, shq(ssid), shq(key));
	} else if (enc == 'none' || enc == '' || enc == 'open') {
		nvunit('akm', 'open');
		nvunit('crypto', 'off');
		wl_cmd('wsec', '0'); wl_cmd('wpa_auth', '0'); wl_cmd('eap', '0');
	} else {
		log(`wl: unsupported encryption '${enc}', use psk2+ccmp or none`);
		return false;
	}

	wl_cmd('chanspec', spec) == 0 || wl_cmd('channel', `${chan}`);
	wl_cmd('up');
	sleep(2000);
	// `up` re-reads nvram and can re-arm power save, which parks the radio;
	// and left to itself the driver can land on a DFS channel and stay silent.
	wl_cmd('mpc', '0');
	wl_cmd('chanspec', spec);

	/* only now, with the driver up: `wl ssid` is what starts the BSS, and
	 * it fails with "Not up" if issued while down. */
	wl_cmd('ssid', shq(ssid));

	fs.mkdir(STATE);
	fs.writefile(`${STATE}/${IFACE}`, nas_args);
	/* Re-read the MAC now that the radio is up: eapd exits 255 if it does not
	 * match wlX_hwaddr, and at boot (only there) the first read, before
	 * `wl up`, seems to get a different one. Log it if it changed. */
	let mac2 = uc(trim(fs.readfile(`/sys/class/net/${IFACE}/address`) ?? ''));
	if (mac2 != mac) {
		log(`wl: ${IFACE} MAC changed after up: ${mac} -> ${mac2}`);
		if (UNIT == 0)
			nvset('wl_hwaddr', mac2);
		nvunit('hwaddr', mac2);
	}
	restart_auth();

	sh(`ip link set ${IFACE} up`);
	netifd.set_vif(section, IFACE);
	return true;
}

function setup() {
	let data = parse_cfg();
	let dcfg = data.config ?? {};
	dcfg.band = radio_band(dcfg);
	let g2 = (dcfg.band == '2g');

	if (dcfg.disabled) {
		netifd.set_up();
		return 0;
	}

	if (!load_module()) {
		netifd.setup_failed('DRIVER_LOAD_FAILED');
		return 1;
	}
	if (g2 && !find_iface('2g') && !bind_pci_radio()) {
		netifd.setup_failed('DRIVER_LOAD_FAILED');
		return 1;
	}
	IFACE = wait_iface(dcfg.band);
	UNIT = g2 ? 1 : 0;
	if (!IFACE) {
		netifd.setup_failed('DRIVER_LOAD_FAILED');
		return 1;
	}
	mknvram();

	let done = false;
	for (let section, iface in data.interfaces) {
		let icfg = iface.config ?? {};
		if ((icfg.mode ?? 'ap') != 'ap') {
			log('wl: only AP mode is supported, skipping');
			continue;
		}
		if (done) {
			log('wl: only one BSS per radio supported, ignoring extra wifi-iface');
			continue;
		}
		if (setup_vif(iface.section ?? section, icfg, dcfg))
			done = true;
	}

	if (!done) {
		netifd.setup_failed('NO_AP_IFACE');
		return 1;
	}

	netifd.set_up();
	return 0;
}

function teardown() {
	let data = parse_cfg();

	IFACE = find_iface(radio_band(data.config));
	if (!IFACE)
		return 0;
	fs.unlink(`${STATE}/${IFACE}`);
	restart_auth();
	sh(`ip link set ${IFACE} down`);
	wl_cmd('down');
	return 0;
}

let ret = 1;
switch (ARGV[1]) {
case 'dump':
	ret = dump_options();
	break;
case 'setup':
	ret = setup();
	break;
case 'teardown':
	ret = teardown();
	break;
}
exit(ret);
