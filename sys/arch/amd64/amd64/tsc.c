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
#include <sys/malloc.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>

#define RECALIBRATE_MAX_RETRIES		5
#define RECALIBRATE_SMI_THRESHOLD	50000
#define RECALIBRATE_DELAY_THRESHOLD	50

int		tsc_recalibrate;

uint64_t	tsc_frequency;
int		tsc_is_invariant;

#define	TSC_DRIFT_MAX			250
#define TSC_SKEW_MAX			100
int64_t	tsc_drift_observed;

volatile int64_t	tsc_sync_val;
volatile struct cpu_info	*tsc_sync_cpu;

u_int		tsc_get_timecount(struct timecounter *tc);
void		tsc_delay(int usecs);

#include "lapic.h"
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
	return rdtsc_lfence() + curcpu()->ci_tsc_skew;
}

void
tsc_timecounter_init(struct cpu_info *ci, uint64_t cpufreq)
{
#ifdef TSC_DEBUG
	printf("%s: TSC skew=%lld observed drift=%lld\n", ci->ci_dev->dv_xname,
	    (long long)ci->ci_tsc_skew, (long long)tsc_drift_observed);
#endif
	if (ci->ci_tsc_skew < -TSC_SKEW_MAX || ci->ci_tsc_skew > TSC_SKEW_MAX) {
		printf("%s: disabling user TSC (skew=%lld)\n",
		    ci->ci_dev->dv_xname, (long long)ci->ci_tsc_skew);
		tsc_timecounter.tc_user = 0;
	}

	if (!(ci->ci_flags & CPUF_PRIMARY) ||
	    !(ci->ci_flags & CPUF_CONST_TSC) ||
	    !(ci->ci_flags & CPUF_INVAR_TSC))
		return;

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

	if (tsc_drift_observed > TSC_DRIFT_MAX) {
		printf("ERROR: %lld cycle TSC drift observed\n",
		    (long long)tsc_drift_observed);
		tsc_timecounter.tc_quality = -1000;
		tsc_timecounter.tc_user = 0;
		tsc_is_invariant = 0;
	}

	tc_init(&tsc_timecounter);
}

/*
 * Record drift (in clock cycles).  Called during AP startup.
 */
void
tsc_sync_drift(int64_t drift)
{
	if (drift < 0)
		drift = -drift;
	if (drift > tsc_drift_observed)
		tsc_drift_observed = drift;
}

/*
 * Called during startup of APs, by the boot processor.  Interrupts
 * are disabled on entry.
 */
void
tsc_read_bp(struct cpu_info *ci, uint64_t *bptscp, uint64_t *aptscp)
{
	uint64_t bptsc;

	if (atomic_swap_ptr(&tsc_sync_cpu, ci) != NULL)
		panic("tsc_sync_bp: 1");

	/* Flag it and read our TSC. */
	atomic_setbits_int(&ci->ci_flags, CPUF_SYNCTSC);
	bptsc = (rdtsc_lfence() >> 1);

	/* Wait for remote to complete, and read ours again. */
	while ((ci->ci_flags & CPUF_SYNCTSC) != 0)
		membar_consumer();
	bptsc += (rdtsc_lfence() >> 1);

	/* Wait for the results to come in. */
	while (tsc_sync_cpu == ci)
		CPU_BUSY_CYCLE();
	if (tsc_sync_cpu != NULL)
		panic("tsc_sync_bp: 2");

	*bptscp = bptsc;
	*aptscp = tsc_sync_val;
}

void
tsc_sync_bp(struct cpu_info *ci)
{
	uint64_t bptsc, aptsc;

	tsc_read_bp(ci, &bptsc, &aptsc); /* discarded - cache effects */
	tsc_read_bp(ci, &bptsc, &aptsc);

	/* Compute final value to adjust for skew. */
	ci->ci_tsc_skew = bptsc - aptsc;
}

/*
 * Called during startup of AP, by the AP itself.  Interrupts are
 * disabled on entry.
 */
void
tsc_post_ap(struct cpu_info *ci)
{
	uint64_t tsc;

	/* Wait for go-ahead from primary. */
	while ((ci->ci_flags & CPUF_SYNCTSC) == 0)
		membar_consumer();
	tsc = (rdtsc_lfence() >> 1);

	/* Instruct primary to read its counter. */
	atomic_clearbits_int(&ci->ci_flags, CPUF_SYNCTSC);
	tsc += (rdtsc_lfence() >> 1);

	/* Post result.  Ensure the whole value goes out atomically. */
	(void)atomic_swap_64(&tsc_sync_val, tsc);

	if (atomic_swap_ptr(&tsc_sync_cpu, NULL) != ci)
		panic("tsc_sync_ap");
}

void
tsc_sync_ap(struct cpu_info *ci)
{
	tsc_post_ap(ci);
	tsc_post_ap(ci);
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


#define TEST_COUNT 10

int tsc_is_ok = 0;
int tsc_ncpus;

#define TSC_READ(x)					\
	static void					\
	tsc_read_##x(struct cpu_info *ci, void *d)	\
	{						\
		uint64_t *array = (uint64_t *)d;	\
		array[ci->ci_cpuid * 3 + x] = rdtsc();	\
	}						\

TSC_READ(0)
TSC_READ(1)
TSC_READ(2)

void
tsc_comp_test(struct cpu_info *ci, void *d)
{
	uint64_t *tsc;
	int64_t d1, d2;
	u_int cpu = ci->ci_cpuid;
	u_int i, j, size;

	size = tsc_ncpus * 3;
	for (i = 0, tsc = d; i < TEST_COUNT; i++, tsc += size)
		for (j = 0; j < tsc_ncpus; j++) {
			if (j == cpu)
				continue;
			d1 = tsc[cpu * 3 + 1] - tsc[j * 3];
			d2 = tsc[cpu * 3 + 2] - tsc[j * 3 + 1];
			if (d1 <= 0 || d2 <= 0) {
				tsc_is_ok = 0;
				return;
			}
		}
}

void
tsc_sync_test(void)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;
	int i, len, ncpus = 0;
	uint64_t *data;
	printf("TSC: sync test start\n");

	CPU_INFO_FOREACH(cii, ci)
		ncpus++;
	tsc_ncpus = ncpus;

	len = sizeof(uint64_t) * ncpus * 3 * TEST_COUNT;
	data = malloc(len, M_TEMP, M_WAITOK);
	memset(data, 0, len);

	for (i = 0; i < TEST_COUNT; i++)
		x86_rendezvous(tsc_read_0,
			       tsc_read_1,
			       tsc_read_2,
			       &data[ncpus * i * 3]);

	tsc_is_ok = 1;
	x86_rendezvous(x86_no_rendezvous_barrier,
		       tsc_comp_test,
		       x86_no_rendezvous_barrier,
		       data);
#if 1
	printf("TSC: results\n");
	for (i = 0; i < TEST_COUNT; i++) {
		uint64_t b;
		b = data[(i * ncpus) * 3];
		printf("  %lld", b);
		for (int j = 1; j < ncpus; j++)
			printf("  %lld", data[(i * ncpus + j) * 3] - b);
		printf("\n");
		b = data[(i * ncpus) * 3 + 1];
		printf("  %lld", b);
		for (int j = 1; j < ncpus; j++)
			printf("  %lld", data[(i * ncpus + j) * 3 + 1] - b);
		printf("\n");
		b = data[(i * ncpus) * 3 + 2];
		printf("  %lld", b);
		for (int j = 1; j < ncpus; j++)
			printf("  %lld", data[(i * ncpus + j) * 3 + 2] - b);
		printf("\n");
	}
#endif
	free(data, M_TEMP, len);
	printf("TSC: sync test end with %d\n", tsc_is_ok);
}
