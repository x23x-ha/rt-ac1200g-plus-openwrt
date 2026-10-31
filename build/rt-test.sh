#!/bin/bash
# Swap b43 on the router to a test module (from /tmp, nothing persisted) and
# measure.  Usage: rt-test.sh <module.ko|stock> [b43 module params...]
#   "stock" reloads /lib/modules/.../b43.ko.  Needs ssh key auth to the router.
#   ROUTER=root@<ip> (default root@192.168.1.1); WSCAN=<scanner> optional:
#   a command printing one line per network, "ch  6 ..." lines are shown.
set -euo pipefail
R=${ROUTER:-root@192.168.1.1}
KO=$1; shift; PARAMS="$*"
SCAN=${WSCAN:-}
ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 "$R" "$@"; }

if [ "$KO" = stock ]; then LOAD="modprobe b43 $PARAMS"
else ssh_ 'cat > /tmp/b43-test.ko' < "$KO"; LOAD="insmod /tmp/b43-test.ko $PARAMS"; fi

ssh_ "echo '=== rt-test: $(basename "$KO") $PARAMS ===' > /dev/kmsg
	wifi down; sleep 2; rmmod b43 2>/dev/null
	$LOAD || { echo LOAD FAILED, back to stock; modprobe b43; }
	sleep 2; wifi up; sleep 25
	dmesg | sed -n '/=== rt-test: /h; /=== rt-test: /,\$H; \${x;p}' | grep -E 'rt-test|dbg:|b43-phy|ERROR|WARN|Oops' | tail -n 25
	iw dev | grep -E 'Interface|txpower'"
[ -n "$SCAN" ] || exit 0
echo "--- scan, channel 6:"
$SCAN | grep -E '^ch  6 ' | sort -u
