/*
 * $QNXLicenseC:
 * Copyright 2008, QNX Software Systems. 
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"). You 
 * may not reproduce, modify or distribute this software except in 
 * compliance with the License. You may obtain a copy of the License 
 * at: http://www.apache.org/licenses/LICENSE-2.0 
 * 
 * Unless required by applicable law or agreed to in writing, software 
 * distributed under the License is distributed on an "AS IS" basis, 
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as 
 * contributors under the License or as licensors under other terms.  
 * Please review this entire file for other proprietary rights or license 
 * notices, as well as the QNX Development Suite License Guide at 
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */

#include "startup.h"

extern struct callout_rtn   timer_load_virt;
extern struct callout_rtn   timer_value_virt;
extern struct callout_rtn   timer_reload_virt;

static const struct callout_slot	timer_callouts[] = {
	{ CALLOUT_SLOT(timer_load, _virt) },
	{ CALLOUT_SLOT(timer_value, _virt) },
	{ CALLOUT_SLOT(timer_reload, _virt) },
};

static unsigned
timer_start_virt()
{
	unsigned lo, hi;

	__asm__ __volatile__("mrrc  p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return lo;
}

static unsigned
timer_diff_virt(unsigned start)
{
	unsigned lo, hi;

	__asm__ __volatile__("mrrc  p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));

	return lo - start;
}

void
init_qtime_virt()
{
	struct qtime_entry	*qtime = alloc_qtime();

	timer_start = timer_start_virt;
	timer_diff  = timer_diff_virt;

	/* MHI2Q: on qemu -M virt the generic timer arrives on GIC PPI 27, not on
	 * GPT1 irq 1. With intr = 1 the guest clock never ticks: procnto gets no
	 * timer interrupt and the whole stack hangs on the first timeout. */
	qtime->intr = 27;	/* was: 1 (GPT1 irq) */
	//invert_timer_freq(qtime, timer_freq);

	/*
	 * Generic timer registers are banked per-cpu so ensure that the
	 * system clock is only operated on via cpu0
	 */
	qtime->flags |= QTIME_FLAG_TIMER_ON_CPU0;
#define	VIRT_CLOCK_FREQ		24000000UL
#define	VIRT_CLOCK_RATE		83333333UL
#define	VIRT_CLOCK_SCALE	-15
	qtime->timer_rate = VIRT_CLOCK_RATE;
	qtime->timer_scale = VIRT_CLOCK_SCALE;
	qtime->cycles_per_sec = (uint64_t)VIRT_CLOCK_FREQ;

	add_callout_array(timer_callouts, sizeof(timer_callouts));
}


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/init_qtime_virt.c $ $Rev: 604842 $" );
