/*	$OpenBSD: tsc.c,v 1.24 2021/08/31 15:11:54 kettenis Exp $	*/
/*
 * Copyright (c) 2008 The NetBSD Foundation, Inc.
 * Copyright (c) 2016,2017 Reyk Floeter <reyk@openbsd.org>
 * Copyright (c) 2017 Adam Steen <adam@adamsteen.com.au>
 * Copyright (c) 2017 Mike Belopuhov <mike@openbsd.org>
 * Copyright (c) 2019 Paul Irofti <paul@irofti.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/timetc.h>
#include <sys/atomic.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>

#define TSC_DEBUG	1

#ifdef TSC_DEBUG
#define DPRINTF(_x...)	do { printf("tsc: " _x); } while (0)
#else
#define DPRINTF(_x...)
#endif

#define RECALIBRATE_MAX_RETRIES		5
#define RECALIBRATE_SMI_THRESHOLD	50000
#define RECALIBRATE_DELAY_THRESHOLD	50

int		tsc_recalibrate;

uint64_t	tsc_frequency;
int		tsc_is_invariant;

u_int		tsc_get_timecount(struct timecounter *tc);
void		tsc_delay(int usecs);

#if NLAPIC > 0
extern u_int32_t lapic_per_second;
#endif

struct timecounter tsc_timecounter = {
	.tc_get_timecount = tsc_get_timecount,
	.tc_poll_pps = NULL,
	.tc_counter_mask = ~0u,
	.tc_frequency = 0,
	.tc_name = "tsc",
	.tc_quality = -1000,
	.tc_priv = NULL,
	.tc_user = TC_TSC,
};

uint64_t
tsc_freq_cpuid(struct cpu_info *ci)
{
	uint64_t count;
	uint32_t eax, ebx, khz, dummy;

	if (!strcmp(cpu_vendor, "GenuineIntel") &&
	    cpuid_level >= 0x15) {
		eax = ebx = khz = dummy = 0;
		CPUID(0x15, eax, ebx, khz, dummy);
		khz /= 1000;
		if (khz == 0) {
			switch (ci->ci_model) {
			case 0x4e: /* Skylake mobile */
			case 0x5e: /* Skylake desktop */
			case 0x8e: /* Kabylake mobile */
			case 0x9e: /* Kabylake desktop */
			case 0xa5: /* CML-H CML-S62 CML-S102 */
			case 0xa6: /* CML-U62 */
				khz = 24000; /* 24.0 MHz */
				break;
			case 0x5f: /* Atom Denverton */
				khz = 25000; /* 25.0 MHz */
				break;
			case 0x5c: /* Atom Goldmont */
				khz = 19200; /* 19.2 MHz */
				break;
			}
		}
		if (ebx == 0 || eax == 0)
			count = 0;
		else if ((count = (uint64_t)khz * (uint64_t)ebx / eax) != 0) {
#if NLAPIC > 0
			lapic_per_second = khz * 1000;
#endif
			return (count * 1000);
		}
	}

	return (0);
}

void
tsc_identify(struct cpu_info *ci)
{
	if (!(ci->ci_flags & CPUF_PRIMARY) ||
	    !(ci->ci_flags & CPUF_CONST_TSC) ||
	    !(ci->ci_flags & CPUF_INVAR_TSC))
		return;

	tsc_is_invariant = 1;

	tsc_frequency = tsc_freq_cpuid(ci);
	if (tsc_frequency > 0)
		delay_func = tsc_delay;
}

static inline int
get_tsc_and_timecount(struct timecounter *tc, uint64_t *tsc, uint64_t *count)
{
	uint64_t n, tsc1, tsc2;
	int i;

	for (i = 0; i < RECALIBRATE_MAX_RETRIES; i++) {
		tsc1 = rdtsc_lfence();
		n = (tc->tc_get_timecount(tc) & tc->tc_counter_mask);
		tsc2 = rdtsc_lfence();

		if ((tsc2 - tsc1) < RECALIBRATE_SMI_THRESHOLD) {
			*count = n;
			*tsc = tsc2;
			return (0);
		}
	}
	return (1);
}

static inline uint64_t
calculate_tsc_freq(uint64_t tsc1, uint64_t tsc2, int usec)
{
	uint64_t delta;

	delta = (tsc2 - tsc1);
	return (delta * 1000000 / usec);
}

static inline uint64_t
calculate_tc_delay(struct timecounter *tc, uint64_t count1, uint64_t count2)
{
	uint64_t delta;

	if (count2 < count1)
		count2 += tc->tc_counter_mask;

	delta = (count2 - count1);
	return (delta * 1000000 / tc->tc_frequency);
}

uint64_t
measure_tsc_freq(struct timecounter *tc)
{
	uint64_t count1, count2, frequency, min_freq, tsc1, tsc2;
	u_long s;
	int delay_usec, i, err1, err2, usec, success = 0;

	/* warmup the timers */
	for (i = 0; i < 3; i++) {
		(void)tc->tc_get_timecount(tc);
		(void)rdtsc();
	}

	min_freq = ULLONG_MAX;

	delay_usec = 100000;
	for (i = 0; i < 3; i++) {
		s = intr_disable();

		err1 = get_tsc_and_timecount(tc, &tsc1, &count1);
		delay(delay_usec);
		err2 = get_tsc_and_timecount(tc, &tsc2, &count2);

		intr_restore(s);

		if (err1 || err2)
			continue;

		usec = calculate_tc_delay(tc, count1, count2);

		if ((usec < (delay_usec - RECALIBRATE_DELAY_THRESHOLD)) ||
		    (usec > (delay_usec + RECALIBRATE_DELAY_THRESHOLD)))
			continue;

		frequency = calculate_tsc_freq(tsc1, tsc2, usec);

		min_freq = MIN(min_freq, frequency);
		success++;
	}

	return (success > 1 ? min_freq : 0);
}

void
calibrate_tsc_freq(void)
{
	struct timecounter *reference = tsc_timecounter.tc_priv;
	uint64_t freq;

	if (!reference || !tsc_recalibrate)
		return;

	if ((freq = measure_tsc_freq(reference)) == 0)
		return;
	tsc_frequency = freq;
	tsc_timecounter.tc_frequency = freq;
	if (tsc_is_invariant)
		tsc_timecounter.tc_quality = 2000;
}

void
cpu_recalibrate_tsc(struct timecounter *tc)
{
	struct timecounter *reference = tsc_timecounter.tc_priv;

	/* Prevent recalibration with a worse timecounter source */
	if (reference && reference->tc_quality > tc->tc_quality)
		return;

	tsc_timecounter.tc_priv = tc;
	calibrate_tsc_freq();
}

u_int
tsc_get_timecount(struct timecounter *tc)
{
	return rdtsc_lfence();
}

void
tsc_timecounter_init(struct cpu_info *ci, uint64_t cpufreq)
{

	KASSERT(CPU_IS_PRIMARY(ci));
	if (!ISSET(ci->ci_flags, CPUF_CONST_TSC)) {
		DPRINTF("cannot use timecounter: not constant\n");
		return;
	}
	if (!ISSET(ci->ci_flags, CPUF_INVAR_TSC)) {
		DPRINTF("cannot use timecounter: not invariant\n");
		return;
	}

	tsc_is_invariant = 1;
	tsc_frequency = tsc_freq_cpuid(ci);

	/* Newer CPUs don't require recalibration */
	if (tsc_frequency > 0) {
		tsc_timecounter.tc_frequency = tsc_frequency;
		tsc_timecounter.tc_quality = 2000;
	} else {
		tsc_recalibrate = 1;
		tsc_frequency = cpufreq;
		tsc_timecounter.tc_frequency = cpufreq;
		calibrate_tsc_freq();
	}

	/*
	 * We can't use the TSC as a timecounter on an MP kernel until
	 * we've checked the synchronization in tsc_check_sync() after
	 * booting the secondary CPUs.
	 *
	 * We can't use the TSC for delay(9), either, because we might
	 * jump the TSC during tsc_check_sync(), which would violate the
	 * assumptions in tsc_delay().
	 */
#ifndef MULTIPROCESSOR
	delay_func = tsc_delay;
	tc_init(&tsc_timecounter);
#endif
}

void
tsc_delay(int usecs)
{
	uint64_t interval, start;

	interval = (uint64_t)usecs * tsc_frequency / 1000000;
	start = rdtsc_lfence();
	while (rdtsc_lfence() - start < interval)
		CPU_BUSY_CYCLE();
}

#ifdef MULTIPROCESSOR

/* TODO how many iterations is "good enough"? */
#define TSC_NREADS	512

/* For sake of testing you can forcibly desync your TSCs. */
/* #define TSC_DESYNC	1 */

#define TSC_ADJUST_ROUNDS	5

/* TODO this should be malloc'd */
uint64_t tsc_data[MAXCPUS][TSC_NREADS];
volatile uint32_t tsc_read_barrier[TSC_NREADS];
volatile uint32_t tsc_compare_barrier;
volatile uint32_t tsc_leave_barrier;
uint32_t tsc_barrier_threshold;
uint32_t tsc_rounds;
int tsc_have_adjust_msr;
volatile int tsc_sync;

/*
 * XXX Sloooow.
 */
static void
selection_sort(int64_t *array, size_t n)
{
	uint64_t tmp;
	size_t i, j, mindex;

	for (i = 0; i < n - 1; i++) {
		mindex = i;
		for (j = i + 1; j < n; j++) {
			if (array[j] < array[mindex])
				mindex = j;
		}
		if (mindex != i) {
			tmp = array[i];
			array[i] = array[mindex];
			array[mindex] = tmp;
		}
	}
}

/*
 * Compute skew between each element of ref and data, then write
 * the skews to data.
 *
 * Then sort data, compute the median, and return it to the caller.
 *
 * The values in ref are not modified by this routine.
 */
static int64_t
find_median_skew(const uint64_t *ref, uint64_t *data, size_t size)
{
	int64_t a, b;
	size_t i, mid;

	/* The unsigned arithmetic isn't an issue here. */
	for (i = 0; i < size; i++)
		data[i] = ref[i] - data[i];

	/*
	 * Find the median skew.  Note that selection_sort() treats
	 * these as signed elements for purposes of comparison.
	 */
	selection_sort((int64_t *)data, size);
	mid = size / 2;
	a = (int64_t)data[mid - 1];
	b = (int64_t)data[mid];

	return a / 2 + b / 2;
}

void
x86_64_ipi_tsc_compare(struct cpu_info *ci)
{
	uint32_t barrier_threshold;
	unsigned int cpu, i, j;
	int sync;

	barrier_threshold = tsc_barrier_threshold;
	cpu = CPU_INFO_UNIT(ci);

	if (tsc_rounds == 0 && tsc_have_adjust_msr) {
		if (rdmsr(MSR_TSC_ADJUST) != 0) {
			DPRINTF("cpu%u: zeroing TSC_ADJUST; was %lld\n",
			    cpu, rdmsr(MSR_TSC_ADJUST));
		}
		wrmsr(MSR_TSC_ADJUST, 0);
	}

#ifdef TSC_DESYNC
	if (tsc_rounds == 0) {
		if (tsc_have_adjust_msr)
			wrmsr(MSR_TSC_ADJUST, arc4random());
		else
			wrmsr_safe(MSR_TSC, rdtsc() + arc4random());
	}
#endif

	/*
	 * Mitigate cache effects on measurement?
	 *
	 * XXX I have no idea what this does.
	 */
	wbinvd();

	for (i = 0; i < nitems(tsc_data[cpu]); i++) {
		atomic_inc_int(&tsc_read_barrier[i]);
		while (tsc_read_barrier[i] != barrier_threshold)
			membar_consumer();
		tsc_data[cpu][i] = rdtsc_lfence();
	}

	atomic_inc_int(&tsc_compare_barrier);
	while (tsc_compare_barrier != barrier_threshold)
		membar_consumer();

	sync = 1;
	for (i = 1; i < nitems(tsc_data[cpu]); i++) {
		for (j = 0; j < nitems(tsc_data); j++) {
			if (tsc_data[cpu][i] <= tsc_data[j][i - 1]) {
				sync = 0;
				break;
			}
		}
		if (!sync)
			break;
	}
	if (!sync) {
		tsc_sync = 0;	/* global, everyone can see this. */
		DPRINTF("cpu%u[%u] lags cpu%u[%u]: %llu <= %llu\n",
		    cpu, i, j, i - 1, tsc_data[cpu][i], tsc_data[j][i - 1]);
	}

	atomic_inc_int(&tsc_leave_barrier);
	while (tsc_leave_barrier != barrier_threshold)
		membar_consumer();
}

void
x86_64_ipi_tsc_adjust(struct cpu_info *ci)
{
	int64_t median;
	uint32_t barrier_threshold;
	unsigned int cpu, ref;
	int failed;

	KASSERT(!CPU_IS_PRIMARY(ci));

	barrier_threshold = tsc_barrier_threshold;
	cpu = CPU_INFO_UNIT(ci);

	/*
	 * Compute median skew from BSP.  Note that this clobbers the
	 * counts we recorded in tsc_data[cpu] earlier.
	 *
	 * XXX Assume BSP is id 0.
	 *
	 * XXX BSP is not necessarily the best reference.  On NUMA
	 * systems we should sync CPUs within a package to another
	 * CPU in the same package to avoid latency error.
	 */
	ref = 0;
	median = find_median_skew(tsc_data[ref], tsc_data[cpu],
	    nitems(tsc_data[ref]));
	DPRINTF("cpu%u: min=%lld max=%lld med=%lld\n",
	    cpu, tsc_data[cpu][0], tsc_data[cpu][nitems(tsc_data[cpu]) - 1],
	    median);

	/*
	 * Try to adjust the local TSC accordingly.
	 *
	 * Writing the TSC MSR is not necessarily allowed, hence
	 * wrmsr_safe().
	 *
	 * Prefer MSR_TSC_ADJUST if we have it.  The write is atomic,
	 * so we avoid the race between the RDTSC and the WRMSR wherein
	 * we could be preempted by a hypervisor, SMM code, etc.
	 *
	 * XXX Maybe we shouldn't try to adjust if the median skew is
	 * within a certain margin?
	 */
	if (tsc_have_adjust_msr) {
		failed = 0;
		wrmsr(MSR_TSC_ADJUST, rdmsr(MSR_TSC_ADJUST) + median);
	} else {
		failed = wrmsr_safe(MSR_TSC, rdtsc_lfence() + median);
		DPRINTF("cpu%u: wrmsr %s\n", cpu, failed ? "failed" : "ok");
	}

	atomic_inc_int(&tsc_leave_barrier);
	while (tsc_leave_barrier != barrier_threshold)
		membar_consumer();
}

void
tsc_check_sync(void)
 {
	struct cpu_info *ci = curcpu();
	unsigned int i, j;

	KASSERT(CPU_IS_PRIMARY(ci));

	if (!tsc_is_invariant)
		return;

	tsc_barrier_threshold = ncpus;	/* XXX can we assume this? */
	if (ISSET(ci->ci_feature_sefflags_ebx, SEFF0EBX_TSC_ADJUST))
		tsc_have_adjust_msr = 1;

	/*
	 * Take measurements.  If we aren't synchronized, try to
	 * adjust everyone.  Then check again.
	 */
	for (i = 0; i < TSC_ADJUST_ROUNDS + 1; i++) {
		/* Reset all barriers. */
		for (j = 0; j < nitems(tsc_read_barrier); j++)
			tsc_read_barrier[j] = 0;
		tsc_compare_barrier = tsc_leave_barrier = 0;

		/* Measure and compare. */
		DPRINTF("testing synchronization...\n");
		tsc_rounds = i;
		tsc_sync = 1;
		x86_broadcast_ipi(X86_IPI_TSC_CMP);
		x86_send_ipi(ci, X86_IPI_TSC_CMP);
		if (tsc_sync)
			break;

		/* Tell everyone to sync up. */
		if (i < TSC_ADJUST_ROUNDS) {
			DPRINTF("not synchronized; adjusting...\n");
			tsc_leave_barrier = 0;
			x86_broadcast_ipi(X86_IPI_TSC_ADJ);
			atomic_inc_int(&tsc_leave_barrier);
			while (tsc_leave_barrier != tsc_barrier_threshold)
				membar_consumer();
		}
	}

#ifdef TSC_DEBUG
	/* Manually print the stats from the final comparison. */
	for (i = 1; i < ncpus; i++) {
		int64_t median = find_median_skew(tsc_data[0], tsc_data[i],
		    nitems(tsc_data[0]));
		DPRINTF("cpu%u: min=%lld max=%lld med=%lld\n",
		    i, tsc_data[i][0], tsc_data[i][nitems(tsc_data[i]) - 1],
		    median);
	}
#endif

	if (tsc_sync) {
		if (tsc_rounds > 0) {
			DPRINTF("synchronized after %u adjustment rounds\n",
			    tsc_rounds);
		} else
			DPRINTF("already synchronized\n");
		tc_init(&tsc_timecounter);
	} else
		DPRINTF("cannot use timecounter: not synchronized\n");

	/*
	 * We can use the TSC for delay(9) even if they aren't sync'd.
	 * The only thing that matters is a constant frequency.
	 */
	delay_func = tsc_delay;
}
#endif /* MULTIPROCESSOR */
