/*	$OpenBSD: ipi.c,v 1.17 2020/01/21 02:01:50 mlarkin Exp $	*/
/*	$NetBSD: ipi.c,v 1.2 2003/03/01 13:05:37 fvdl Exp $	*/

/*-
 * Copyright (c) 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by RedBack Networks Inc.
 *
 * Author: Bill Sommerfeld
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/atomic.h>

#include <machine/intr.h>
#include <machine/atomic.h>
#include <machine/cpuvar.h>
#include <machine/cpu.h>
#include <machine/i82489reg.h>
#include <machine/i82489var.h>

void
x86_send_ipi(struct cpu_info *ci, int ipimask)
{
	x86_atomic_setbits_u32(&ci->ci_ipis, ipimask);

	/* Don't send IPI to cpu which isn't (yet) running. */
	if (!(ci->ci_flags & CPUF_RUNNING))
		return;

	x86_ipi(LAPIC_IPI_VECTOR, ci->ci_apicid, LAPIC_DLMODE_FIXED);
}

int
x86_fast_ipi(struct cpu_info *ci, int ipi)
{
	if (!(ci->ci_flags & CPUF_RUNNING))
		return (ENOENT);

	x86_ipi(ipi, ci->ci_apicid, LAPIC_DLMODE_FIXED);

	return 0;
}

void
x86_broadcast_ipi(int ipimask)
{
	struct cpu_info *ci, *self = curcpu();
	int count = 0;
	CPU_INFO_ITERATOR cii;

	CPU_INFO_FOREACH(cii, ci) {
		if (ci == self)
			continue;
		if ((ci->ci_flags & CPUF_RUNNING) == 0)
			continue;
		x86_atomic_setbits_u32(&ci->ci_ipis, ipimask);
		count++;
	}
	if (!count)
		return;

	x86_ipi(LAPIC_IPI_VECTOR, LAPIC_DEST_ALLEXCL, LAPIC_DLMODE_FIXED);
}

void
x86_ipi_handler(void)
{
	extern struct evcount ipi_count;
	struct cpu_info *ci = curcpu();
	u_int32_t pending;
	int bit;
	int floor;

	floor = ci->ci_handled_intr_level;
	ci->ci_handled_intr_level = ci->ci_ilevel;

	pending = atomic_swap_uint(&ci->ci_ipis, 0);
	for (bit = 0; bit < X86_NIPI && pending; bit++) {
		if (pending & (1 << bit)) {
			pending &= ~(1 << bit);
			(*ipifunc[bit])(ci);
			ipi_count.ec_count++;
		}
	}

	ci->ci_handled_intr_level = floor;
}

/* Variables needed for X86 rendezvous. */
static volatile int x86_rv_ncpus;
static void (*volatile x86_rv_setup_func)(struct cpu_info *ci, void *arg);
static void (*volatile x86_rv_action_func)(struct cpu_info *ci, void *arg);
static void (*volatile x86_rv_teardown_func)(struct cpu_info *ci, void *arg);
static void *volatile x86_rv_func_arg;
static volatile int x86_rv_waiters[4];

/*
 * Mutex for x86_rendezvous().
 */
struct mutex x86_ipi_mtx = MUTEX_INITIALIZER(IPL_HIGH);

void
x86_no_rendezvous_barrier(struct cpu_info *ci, void *dummy)
{
	KASSERT(0);
}

void
x86_rendezvous_action(struct cpu_info *ci)
{
	void *local_func_arg;
	void (*local_setup_func)(struct cpu_info *, void*);
	void (*local_action_func)(struct cpu_info *, void*);
	void (*local_teardown_func)(struct cpu_info *, void*);

	/* Ensure we have up-to-date values. */
	atomic_add_int(&x86_rv_waiters[0], 1);
	while (x86_rv_waiters[0] < x86_rv_ncpus)
		CPU_BUSY_CYCLE();

	/* Fetch rendezvous parameters after acquire barrier. */
	local_func_arg = x86_rv_func_arg;
	local_setup_func = x86_rv_setup_func;
	local_action_func = x86_rv_action_func;
	local_teardown_func = x86_rv_teardown_func;

	/*
	 * If requested, run a setup function before the main action
	 * function.  Ensure all CPUs have completed the setup
	 * function before moving on to the action function.
	 */
	if (local_setup_func != x86_no_rendezvous_barrier) {
		if (local_setup_func != NULL)
			local_setup_func(ci, local_func_arg);
		atomic_add_int(&x86_rv_waiters[1], 1);
		while (x86_rv_waiters[1] < x86_rv_ncpus)
			CPU_BUSY_CYCLE();
	}

	if (local_action_func != NULL)
		local_action_func(ci, local_func_arg);


	if (local_teardown_func != x86_no_rendezvous_barrier) {
		/*
		 * Signal that the main action has been completed.  If a
		 * full exit rendezvous is requested, then all CPUs will
		 * wait here until all CPUs have finished the main action.
		 */
		atomic_add_int(&x86_rv_waiters[2], 1);
		while (x86_rv_waiters[2] < x86_rv_ncpus)
			CPU_BUSY_CYCLE();

		if (local_teardown_func != NULL)
			local_teardown_func(ci, local_func_arg);
	}

	/*
	 * Signal that the rendezvous is fully completed by this CPU.
	 * This means that no member of x86_rv_* pseudo-structure will be
	 * accessed by this target CPU after this point; in particular,
	 * memory pointed by x86_rv_func_arg.
	 *
	 * The release semantic ensures that all accesses performed by
	 * the current CPU are visible when x86_rendezvous_cpus()
	 * returns, by synchronizing with the
	 * atomic_load_int(&x86_rv_waiters[3]).
	 */
	atomic_add_int(&x86_rv_waiters[3], 1);
}

void
x86_rendezvous(void (* setup_func)(struct cpu_info *, void *),
	       void (* action_func)(struct cpu_info *, void *),
	       void (* teardown_func)(struct cpu_info *, void *),
	       void *arg)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci, *self = curcpu();
	int ncpus = 0;

	CPU_INFO_FOREACH(cii, ci)
		ncpus++;
	if (ncpus == 0)
		return;

	mtx_enter(&x86_ipi_mtx);

	/* Pass rendezvous parameters via global variables. */
	x86_rv_ncpus = ncpus;
	x86_rv_setup_func = setup_func;
	x86_rv_action_func = action_func;
	x86_rv_teardown_func = teardown_func;
	x86_rv_func_arg = arg;
	x86_rv_waiters[1] = 0;
	x86_rv_waiters[2] = 0;
	x86_rv_waiters[3] = 0;
	atomic_store_int(&x86_rv_waiters[0], 0);

	x86_broadcast_ipi(X86_IPI_RENDEZVOUS);

	/*
	 * Call x86_rendezvous_action for myself.
	 */
	x86_rendezvous_action(self);

	/*
	 * Ensure that the master CPU waits for all the other
	 * CPUs to finish the rendezvous, so that x86_rv_*
	 * pseudo-structure and the arg are guaranteed to not
	 * be in use.
	 *
	 * Load acquire synchronizes with the release add in
	 * x86_rendezvous_action(), which ensures that our caller sees
	 * all memory actions done by the called functions on other
	 * CPUs.
	 */
	while (atomic_load_int(&x86_rv_waiters[3]) < ncpus)
		CPU_BUSY_CYCLE();

	mtx_leave(&x86_ipi_mtx);
}
