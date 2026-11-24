/*
 * pmtel: read IOReport counters, e.g. the PMC telemetry group
 * pmtelemetry_cpu / cstate_counter, to see what keeps the package out of
 * PC10 during a Deep Idle sleep (docs/TESTING.md "Finding what blocks PC10").
 *
 * Method and the original reader: keenerdavid18 (S0ixStandby issue #7).
 * This is S0ixStandby's own implementation of the same idea.
 *
 * Build (macOS):
 *   clang -O2 -o pmtel tools/pmtel.c -framework CoreFoundation -lIOReport
 *
 * Usage:
 *   pmtel list [group]                     groups / subgroups / channels
 *   pmtel snap group [subgroup]            raw values, one "subgroup/channel value" per line
 *   pmtel rate seconds group [subgroup]    per-second rates while awake
 *                                          (sampled every 0.25 s, wrap-aware)
 *   pmtel diff before.txt after.txt [bits] deltas between two snaps,
 *                                          (after - before) mod 2^bits (default 16)
 *
 * IOReport is a private macOS library; the declarations and the way samples
 * are read (the IOReportChannels array) follow keenerdavid18's reader, which
 * is known to work on Intel hacks. Nothing here writes to hardware.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct IOReportSubscription *IOReportSubscriptionRef;

extern CFDictionaryRef IOReportCopyAllChannels(uint64_t, uint64_t);
extern CFDictionaryRef IOReportCopyChannelsInGroup(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
extern IOReportSubscriptionRef IOReportCreateSubscription(void *, CFMutableDictionaryRef,
                                                          CFMutableDictionaryRef *, uint64_t, CFTypeRef);
extern CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
extern CFStringRef IOReportChannelGetGroup(CFDictionaryRef);
extern CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef);
extern CFStringRef IOReportChannelGetChannelName(CFDictionaryRef);
extern int IOReportChannelGetFormat(CFDictionaryRef);
extern int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef, int32_t *);

enum { kMaxCh = 2048, kNameLen = 160 };

typedef struct {
	int n;
	char name[kMaxCh][kNameLen];
	uint64_t val[kMaxCh];
} Set;

static void cfstr(CFStringRef s, char *out, size_t size)
{
	out[0] = 0;
	if (s && !CFStringGetCString(s, out, (CFIndex)size, kCFStringEncodingUTF8))
		snprintf(out, size, "?");
}

// Samples and channel lists are dictionaries with an "IOReportChannels" array.
static CFArrayRef channels(CFDictionaryRef d, CFIndex *n)
{
	CFArrayRef a = d ? (CFArrayRef)CFDictionaryGetValue(d, CFSTR("IOReportChannels")) : NULL;
	*n = a ? CFArrayGetCount(a) : 0;
	return a;
}

static int find(const Set *s, const char *name)
{
	for (int i = 0; i < s->n; i++)
		if (!strcmp(s->name[i], name)) return i;
	return -1;
}

static void usage(void)
{
	fprintf(stderr,
	        "usage: pmtel list [group]\n"
	        "       pmtel snap group [subgroup]\n"
	        "       pmtel rate seconds group [subgroup]\n"
	        "       pmtel diff before.txt after.txt [bits]\n");
	exit(2);
}

static int cmdList(const char *group)
{
	CFDictionaryRef all = IOReportCopyAllChannels(0, 0);
	if (!all) { fprintf(stderr, "pmtel: IOReport returned no channels\n"); return 1; }
	int count = 0;
	CFIndex n;
	CFArrayRef arr = channels(all, &n);
	for (CFIndex k = 0; k < n; k++) {
		CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, k);
		char g[kNameLen], sg[kNameLen], nm[kNameLen];
		cfstr(IOReportChannelGetGroup(ch), g, sizeof(g));
		if (group && strcmp(g, group)) continue;
		cfstr(IOReportChannelGetSubGroup(ch), sg, sizeof(sg));
		cfstr(IOReportChannelGetChannelName(ch), nm, sizeof(nm));
		printf("%s / %s / %s  (fmt %d)\n", g, sg[0] ? sg : "-", nm, IOReportChannelGetFormat(ch));
		count++;
	}
	CFRelease(all);
	if (!count) fprintf(stderr, "pmtel: no channels%s%s\n", group ? " in group " : "", group ? group : "");
	return count ? 0 : 1;
}

typedef struct {
	IOReportSubscriptionRef sub;
	CFMutableDictionaryRef chans;
	CFMutableDictionaryRef subbed;
	const char *subgroup;   // filter: only channels of this subgroup
} Reader;

static int openReader(Reader *r, const char *group, const char *subgroup)
{
	memset(r, 0, sizeof(*r));
	CFStringRef g = CFStringCreateWithCString(NULL, group, kCFStringEncodingUTF8);
	CFDictionaryRef c = IOReportCopyChannelsInGroup(g, NULL, 0, 0, 0);
	CFRelease(g);
	if (!c) { fprintf(stderr, "pmtel: no channels in group %s\n", group); return 0; }
	r->subgroup = subgroup;
	r->chans = CFDictionaryCreateMutableCopy(NULL, 0, c);
	CFRelease(c);
	r->sub = IOReportCreateSubscription(NULL, r->chans, &r->subbed, 0, NULL);
	if (!r->sub) { fprintf(stderr, "pmtel: IOReport subscription failed\n"); return 0; }
	return 1;
}

static void sample(Reader *r, Set *s)
{
	s->n = 0;
	CFDictionaryRef smp = IOReportCreateSamples(r->sub, r->subbed, NULL);
	if (!smp) return;
	CFIndex n;
	CFArrayRef arr = channels(smp, &n);
	for (CFIndex k = 0; k < n && s->n < kMaxCh; k++) {
		CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, k);
		char sg[kNameLen], nm[kNameLen];
		cfstr(IOReportChannelGetSubGroup(ch), sg, sizeof(sg));
		if (r->subgroup && strcmp(sg, r->subgroup)) continue;
		cfstr(IOReportChannelGetChannelName(ch), nm, sizeof(nm));
		snprintf(s->name[s->n], kNameLen, "%s/%s", sg[0] ? sg : "-", nm);
		s->val[s->n] = (uint64_t)IOReportSimpleGetIntegerValue(ch, NULL);
		s->n++;
	}
	CFRelease(smp);
}

static int cmdSnap(const char *group, const char *subgroup)
{
	Reader r;
	if (!openReader(&r, group, subgroup)) return 1;
	static Set s;
	sample(&r, &s);
	if (!s.n) { fprintf(stderr, "pmtel: no counters (check the subgroup with: pmtel list GROUP)\n"); return 1; }
	for (int i = 0; i < s.n; i++)
		printf("%s %llu\n", s.name[i], (unsigned long long)s.val[i]);
	return 0;
}

static int cmdRate(double seconds, const char *group, const char *subgroup)
{
	Reader r;
	if (!openReader(&r, group, subgroup)) return 1;
	static Set prev, cur;
	static uint64_t sum[kMaxCh];
	const uint64_t mask = 0xFFFF;   // counters are 16-bit; works for wider ones too
	sample(&r, &prev);
	if (!prev.n) { fprintf(stderr, "pmtel: no counters (check the subgroup with: pmtel list GROUP)\n"); return 1; }
	int steps = (int)(seconds / 0.25 + 0.5);
	if (steps < 1) steps = 1;
	for (int k = 0; k < steps; k++) {
		usleep(250000);
		sample(&r, &cur);
		for (int i = 0; i < prev.n; i++) {
			int j = find(&cur, prev.name[i]);
			if (j >= 0) sum[i] += (cur.val[j] - prev.val[i]) & mask;
		}
		for (int i = 0; i < prev.n; i++) {
			int j = find(&cur, prev.name[i]);
			if (j >= 0) prev.val[i] = cur.val[j];
		}
	}
	double t = steps * 0.25;
	for (int i = 0; i < prev.n; i++)
		printf("%s %.1f/s\n", prev.name[i], (double)sum[i] / t);
	return 0;
}

static int load(const char *path, Set *s)
{
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); return 0; }
	char line[kNameLen + 40];
	s->n = 0;
	while (s->n < kMaxCh && fgets(line, sizeof(line), f)) {
		char *sp = strrchr(line, ' ');
		if (!sp) continue;
		*sp = 0;
		snprintf(s->name[s->n], kNameLen, "%s", line);
		s->val[s->n] = strtoull(sp + 1, NULL, 10);
		s->n++;
	}
	fclose(f);
	return 1;
}

static int cmdDiff(const char *a, const char *b, int bits)
{
	static Set before, after;
	if (!load(a, &before) || !load(b, &after)) return 1;
	uint64_t mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
	for (int i = 0; i < after.n; i++) {
		int j = find(&before, after.name[i]);
		if (j < 0) { printf("%-48s (not in %s)\n", after.name[i], a); continue; }
		uint64_t d = (after.val[i] - before.val[j]) & mask;
		char ds[32];
		snprintf(ds, sizeof(ds), "+%llu", (unsigned long long)d);
		printf("%-48s %8s  (%llu -> %llu)\n", after.name[i], ds,
		       (unsigned long long)before.val[j], (unsigned long long)after.val[i]);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) usage();
	const char *cmd = argv[1];
	if (!strcmp(cmd, "list")) return cmdList(argc > 2 ? argv[2] : NULL);
	if (!strcmp(cmd, "snap") && argc >= 3) return cmdSnap(argv[2], argc > 3 ? argv[3] : NULL);
	if (!strcmp(cmd, "rate") && argc >= 4) {
		double s = atof(argv[2]);
		if (s <= 0) usage();
		return cmdRate(s, argv[3], argc > 4 ? argv[4] : NULL);
	}
	if (!strcmp(cmd, "diff") && argc >= 4) {
		int bits = argc > 4 ? atoi(argv[4]) : 16;
		if (bits < 1 || bits > 64) usage();
		return cmdDiff(argv[2], argv[3], bits);
	}
	usage();
	return 2;
}
