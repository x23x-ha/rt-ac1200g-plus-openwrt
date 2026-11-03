#!/usr/bin/ucode
// Multiplexing wireless driver entry point for netifd.
//
// netifd's handler_load() (/lib/netifd/utils.uc) can only ever process ONE
// script in /lib/netifd/wireless/: on the second file its read loop never sees
// an error at EOF and spins forever, wedging netifd before it configures any
// interface. Verified by putting two identical copies of mac80211.sh there.
//
// But that same loop parses one JSON object PER LINE and registers a handler
// for each, and netifd later invokes the script with the driver type in
// ARGV[0] (see wireless-device.uc: argv = [script, config.type, op, name, cfg]).
// So one script can serve several drivers. This is that script: it advertises
// both mac80211 and wl, and dispatches to the real implementation.
'use strict';

const ORIG = '/usr/libexec/wl/mac80211.orig.sh';
const WLDRV = '/usr/libexec/wl/wl-driver.uc';

function shq(s) {
	return "'" + replace(`${s ?? ''}`, "'", "'\\''") + "'";
}

function run(prog) {
	return system(`${prog} ${shq(ARGV[0])} ${shq(ARGV[1])} ${shq(ARGV[2])} ${shq(ARGV[3])}`);
}

if (ARGV[1] == 'dump') {
	// one JSON object per line: netifd registers a handler for each
	system(`${ORIG} '' dump`);
	system(`${WLDRV} '' dump`);
	exit(0);
}

exit(run(ARGV[0] == 'broadcom' ? WLDRV : ORIG));
