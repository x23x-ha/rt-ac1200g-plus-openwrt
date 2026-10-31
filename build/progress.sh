#!/bin/bash
# Build progress for run-build.sh.  ./progress.sh = one line, ./progress.sh -f = follow.
LOG=$(cd "$(dirname "$0")" && pwd)/out/make.log
START_FILE=/tmp/owrt-b43-build.start

once() {
	local C; C=$(docker ps -q --filter ancestor=owrt-sdk-env | head -1)
	if [ -z "$C" ]; then echo "no build container running"; tail -n 3 "$LOG" 2>/dev/null; return 1; fi
	[ -f "$START_FILE" ] || date +%s > "$START_FILE"
	local now start el; now=$(date +%s); start=$(cat "$START_FILE"); el=$((now - start))
	# objects built / estimated total (compat + cfg80211 + mac80211 + b43 sources)
	read -r done total < <(docker exec "$C" bash -c '
		B=$(ls -d /work/sdk/build_dir/target-*/linux-*/mac80211-*/backports-* 2>/dev/null | head -1)
		[ -z "$B" ] && { echo "0 0"; exit; }
		d=$(find "$B"/compat "$B"/net/wireless "$B"/net/mac80211 "$B"/drivers/net/wireless/broadcom/b43 -name "*.o" ! -name "*.mod.o" 2>/dev/null | wc -l)
		t=$(find "$B"/compat "$B"/net/wireless "$B"/net/mac80211 "$B"/drivers/net/wireless/broadcom/b43 -maxdepth 1 -name "*.c" 2>/dev/null | wc -l)
		echo "$d $t"')
	local phase
	if [ "$total" = 0 ]; then phase="preparing (packaging deps / downloading backports)"
	elif grep -q "Packaged contents\|kmod-b43.*apk" "$LOG" 2>/dev/null && [ "$done" -gt 0 ]; then phase="packaging"
	else phase="compiling"; fi
	local pct=0 eta="?"
	if [ "$total" -gt 0 ]; then
		pct=$((done * 100 / total)); [ $pct -gt 99 ] && pct=99
		if [ -f /tmp/owrt-b43-build.cc0 ]; then
			read -r t0 d0 < /tmp/owrt-b43-build.cc0
			local dt=$((now - t0)) dd=$((done - d0))
			[ $dd -gt 0 ] && [ "$done" -lt "$total" ] && eta="$(( (total - done) * dt / dd / 60 ))m$(( (total - done) * dt / dd % 60 ))s"
		elif [ "$done" -gt 0 ]; then echo "$now $done" > /tmp/owrt-b43-build.cc0; fi
	fi
	printf '%s  elapsed %dm%02ds  %-50s  %s/%s objs (%s%%)  ETA %s\n' \
		"$(date +%H:%M:%S)" $((el / 60)) $((el % 60)) "$phase" "$done" "$total" "$pct" "$eta"
}

if [ "${1:-}" = -f ]; then while once; do sleep 15; done; else once; fi
