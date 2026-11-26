#!/bin/bash
# pmc-sleep-diff.sh: PMC telemetry counters across one RTC-timed sleep, to see
# what keeps the package out of PC10 (docs/TESTING.md "Finding what blocks
# PC10"). Method by keenerdavid18 (#7); uses tools/pmtel.c.
#   sudo bash macos/pmc-sleep-diff.sh [seconds asleep, default 240]
# Hands off the machine until it wakes by itself. Writes ./pmc-diff-<date>/.
# Group/subgroup can be changed with PMTEL_GROUP / PMTEL_SUBGROUP.

[ "$(id -u)" = 0 ] || { echo "Run with sudo."; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
SECS=${1:-240}
GROUP=${PMTEL_GROUP:-pmtelemetry_cpu}
SUB=${PMTEL_SUBGROUP:-cstate_counter}
OUT="pmc-diff-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 1

PMTEL="$HERE/../tools/pmtel"
if [ ! -x "$PMTEL" ]; then
	PMTEL="$OUT/pmtel"
	clang -O2 -o "$PMTEL" "$HERE/../tools/pmtel.c" -framework CoreFoundation -lIOReport \
		|| { echo "Could not build tools/pmtel.c (needs the Xcode command line tools)."; exit 1; }
fi

"$PMTEL" snap "$GROUP" "$SUB" > "$OUT/check.txt" || {
	echo "No '$GROUP / $SUB' counters on this machine. Available groups:"
	"$PMTEL" list | cut -d/ -f1 | sort -u
	exit 1
}

echo "Awake-idle rates (10 s, don't touch the machine)..."
"$PMTEL" rate 10 "$GROUP" "$SUB" > "$OUT/awake-rate.txt"

# A scheduled macOS wake (maintenance, calendar) inside the test ends it early.
echo "Scheduled wakes (one within $SECS s would end the sleep early):"
pmset -g sched | sed 's/^/  /'
echo "Sleeping for $SECS s with an RTC wake. Hands off until it wakes."
WT0=$(sysctl -n kern.waketime)
"$PMTEL" snap "$GROUP" "$SUB" > "$OUT/before.txt"
pmset relative wake "$SECS" >/dev/null
pmset sleepnow >/dev/null
# Wait for the wake (kern.waketime changes), at most SECS + 10 min.
for _ in $(seq 1 $(( (SECS + 600) / 2 ))); do
	sleep 2
	[ "$(sysctl -n kern.waketime)" != "$WT0" ] && break
done
"$PMTEL" snap "$GROUP" "$SUB" > "$OUT/after.txt"
"$PMTEL" diff "$OUT/before.txt" "$OUT/after.txt" > "$OUT/diff.txt"

{
	echo "== $GROUP / $SUB across one sleep ($SECS s requested)"
	sysctl -n machdep.xcpm.deep_idle_last_stats 2>/dev/null | sed 's/^/deep_idle_last_stats: /'
	echo
	echo "== Deltas (after - before, 16-bit; includes ~10-20 s awake around the sleep)"
	cat "$OUT/diff.txt"
	echo
	echo "== Awake-idle rates (subtract rate x awake seconds from the deltas)"
	cat "$OUT/awake-rate.txt"
} | tee "$OUT/summary.txt"
echo
echo "Saved in $OUT/ (summary.txt). Counters climbing during the halt: see docs/TESTING.md."
