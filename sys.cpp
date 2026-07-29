#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include "htab.h"

int yak_verbose = 3;

static double yak_realtime0;

double yak_cputime(void)
{
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
	return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
}

static inline double yak_realtime_core(void)
{
	struct timeval tp;
	struct timezone tzp;
	gettimeofday(&tp, &tzp);
	return tp.tv_sec + tp.tv_usec * 1e-6;
}

void yak_reset_realtime(void)
{
	yak_realtime0 = yak_realtime_core();
}

double yak_realtime(void)
{
	return yak_realtime_core() - yak_realtime0;
}

double yak_realtime_0(void)
{
	return yak_realtime_core();
}


long yak_peakrss(void)
{
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
#ifdef __linux__
	return r.ru_maxrss * 1024;
#else
	return r.ru_maxrss;
#endif
}

double yak_peakrss_in_gb(void)
{
	return yak_peakrss() / 1073741824.0;
}

double yak_cpu_usage(void)
{
	return (yak_cputime() + 1e-9) / (yak_realtime() + 1e-9);
}

// ---------------------------------------------------------------------------
// Hierarchical stage profiler.
//
// A tiny stack-based profiler for the top-level assembly pipeline. Regions are
// opened with ha_prof_enter() and closed with ha_prof_leave(); nesting is taken
// from the enter/leave stack, so the parent of a region is simply whatever
// region is currently open. Time is *accumulated* per region across repeated
// enters (e.g. the per-round error-correction stages), and ha_prof_report()
// renders the whole thing as an indented tree with each region's wall time and
// its share of the total run time.
//
// It is only driven from the main thread at coarse stage boundaries, so it does
// not need locking and adds no measurable overhead.
// ---------------------------------------------------------------------------
#define HA_PROF_MAX_NODES 64
#define HA_PROF_MAX_DEPTH 16

typedef struct {
	const char *name;
	int parent;        // index of enclosing region, -1 for top level
	double wall, cpu;  // accumulated seconds
	double rt0, ct0;   // start of the currently-open interval
	int used;
} ha_prof_node_t;

static ha_prof_node_t ha_prof_nodes[HA_PROF_MAX_NODES];
static int ha_prof_n = 0;
static int ha_prof_stack[HA_PROF_MAX_DEPTH];
static int ha_prof_sp = 0;

void ha_prof_enter(const char *name)
{
	int parent = ha_prof_sp > 0 ? ha_prof_stack[ha_prof_sp - 1] : -1;
	int i, id = -1;
	for (i = 0; i < ha_prof_n; ++i)
		if (ha_prof_nodes[i].parent == parent && strcmp(ha_prof_nodes[i].name, name) == 0) { id = i; break; }
	if (id < 0 && ha_prof_n < HA_PROF_MAX_NODES) {
		id = ha_prof_n++;
		ha_prof_nodes[id].name = name;
		ha_prof_nodes[id].parent = parent;
		ha_prof_nodes[id].wall = ha_prof_nodes[id].cpu = 0.0;
		ha_prof_nodes[id].used = 0;
	}
	if (id >= 0) {
		ha_prof_nodes[id].rt0 = yak_realtime();
		ha_prof_nodes[id].ct0 = yak_cputime();
	}
	if (ha_prof_sp < HA_PROF_MAX_DEPTH) ha_prof_stack[ha_prof_sp++] = id;
}

void ha_prof_leave(void)
{
	int id;
	if (ha_prof_sp <= 0) return;
	id = ha_prof_stack[--ha_prof_sp];
	if (id < 0) return;
	ha_prof_nodes[id].wall += yak_realtime() - ha_prof_nodes[id].rt0;
	ha_prof_nodes[id].cpu  += yak_cputime()  - ha_prof_nodes[id].ct0;
	ha_prof_nodes[id].used = 1;
}

void ha_prof_report(void)
{
	double total = yak_realtime();
	int i, s, d, p;
	if (ha_prof_n == 0) return;
	fprintf(stderr, "\n[hifiasm] ===== timing breakdown (%.3f sec total) =====\n", total);
	for (i = 0; i < ha_prof_n; ++i) {
		if (!ha_prof_nodes[i].used) continue;
		for (d = 0, p = ha_prof_nodes[i].parent; p >= 0; p = ha_prof_nodes[p].parent) ++d;
		fprintf(stderr, "[hifiasm] ");
		for (s = 0; s < d; ++s) fprintf(stderr, "    ");
		if (d > 0) fprintf(stderr, "- ");
		fprintf(stderr, "%s: %.3f sec (%.1f%%, %.1fx cpu)\n",
				ha_prof_nodes[i].name, ha_prof_nodes[i].wall,
				100.0 * ha_prof_nodes[i].wall / (total + 1e-9),
				ha_prof_nodes[i].cpu / (ha_prof_nodes[i].wall + 1e-9));
	}
}
