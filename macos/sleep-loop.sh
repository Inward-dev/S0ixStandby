#!/bin/bash
# sleep-loop.sh: repeat RTC-timed Deep Idle sleeps and log each result.
#   sudo bash macos/sleep-loop.sh [seconds asleep, default 240] [count, default 5] [pause awake, default 60]
# Each round: schedule an RTC wake, sleep, wait for the wake, then record the
# battery, SLP_S0 residency, C-states, S0ixStandby's sleep trace and any new
# panic / GPU-restart report. Stops at the first new report. A hang stops the
# script with the machine; the log (written and synced each round) shows how
# far it got: ./sleep-loop-<date>.log
# Hands off the machine while it runs. On AC with -s0ixacnortc, RTC wakes are
# blocked on purpose: run it on battery.

[ "$(id -u)" = 0 ] || { echo "Run with sudo."; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
SECS=${1:-240}
COUNT=${2:-5}
PAUSE=${3:-60}
LOG="sleep-loop-$(date +%Y%m%d-%H%M%S).log"
REPORTS=/Library/Logs/DiagnosticReports

say() { echo "$*" | tee -a "$LOG"; sync; }

newest_report() {
	find "$REPORTS" -maxdepth 1 \( -name '*.panic' -o -name '*.gpuRestart' \) -newer "$1" 2>/dev/null | head -1
}

prop() {   # value of one S0ixStandby property
	ioreg -rd1 -c S0ixStandby -w0 | sed -n "s/^ *\"$1\" = //p" | head -1
}

batt() { pmset -g batt | grep -Eo '[0-9]+%;[^;]*' | head -1; }

if pmset -g batt | grep -q "AC Power" && nvram boot-args 2>/dev/null | grep -q -- -s0ixacnortc; then
	echo "On AC with -s0ixacnortc: RTC wakes are blocked. Unplug or remove the flag."; exit 1
fi

say "== sleep-loop $(date): $COUNT x ${SECS} s asleep, ${PAUSE} s awake between"
say "kexts: $(kextstat 2>/dev/null | grep -Eo '(S0ixStandby|WhateverXe) \([0-9.]+\)' | tr '\n' ' ')"
say "boot-args: $(nvram boot-args 2>/dev/null | cut -f2-)"
say "scheduled wakes (one inside a sleep ends it early):"
pmset -g sched | sed 's/^/  /' | tee -a "$LOG"
STAMP=$(mktemp)

for i in $(seq 1 "$COUNT"); do
	say ""
	say "-- round $i/$COUNT $(date '+%H:%M:%S')  battery $(batt)"
	WT0=$(sysctl -n kern.waketime)
	pmset relative wake "$SECS" >/dev/null
	say "   sleeping (if the log ends here, it hung at sleep or in the halt)"
	pmset sleepnow >/dev/null
	woke=0
	for _ in $(seq 1 $(( (SECS + 600) / 2 ))); do
		sleep 2
		[ "$(sysctl -n kern.waketime)" != "$WT0" ] && { woke=1; break; }
	done
	if [ "$woke" = 0 ]; then
		say "   no wake seen within $((SECS + 600)) s; stopping"; break
	fi
	sleep 5   # let S0ixStandby publish (publish timer after the wake)
	say "   woke $(date '+%H:%M:%S')  battery $(batt)"
	say "   SLP_S0 $(prop 'Last residency us') us, C-states $(prop 'Last C-state percent')"
	say "   wake source $(prop 'Last wake source'), trace $(prop 'Sleep trace now')"
	r=$(newest_report "$STAMP")
	if [ -n "$r" ]; then
		say "   NEW REPORT: $r; stopping (run wake-report.sh)"; break
	fi
	if [ "$i" -lt "$COUNT" ]; then
		say "   awake for ${PAUSE} s (use it: a video, a display sleep)"
		sleep "$PAUSE"
		r=$(newest_report "$STAMP")
		[ -n "$r" ] && { say "   NEW REPORT while awake: $r; stopping"; break; }
	fi
done
rm -f "$STAMP"
say ""
say "== done $(date). Log: $LOG"
[ -x "$HERE/../tools/redact.sh" ] || chmod +x "$HERE/../tools/redact.sh" 2>/dev/null
bash "$HERE/../tools/redact.sh" "$LOG" >/dev/null 2>&1 && rm -f "$LOG.bak"
