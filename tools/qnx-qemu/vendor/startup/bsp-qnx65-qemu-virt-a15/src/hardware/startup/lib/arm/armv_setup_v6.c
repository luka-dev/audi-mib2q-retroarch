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

/*
 * Generic ARMv6 CPU initialisation
 */

void
armv_setup_v6(struct cpuinfo_entry *cpu, unsigned cpuid, unsigned cpunum)
{
	unsigned	tmp;

	/*
	 * Check for presence of VFP (cp10/11) and enable access if present
	 */
	tmp = arm_v6_cp15_cpacr_get();
	tmp |= ARM_V6_CPACR_VFP;
	arm_v6_cp15_cpacr_set(tmp);

	/*
	 * Access bits will not be written if coprocessor is not present
	 */
	tmp = arm_v6_cp15_cpacr_get();	
	if ((tmp & ARM_V6_CPACR_VFP) == ARM_V6_CPACR_VFP) {
		/*
		 * Indicate VFP unit is present
		 */
		cpu->flags |= CPU_FLAG_FPU;
		if (debug_flag) {
			__asm__ __volatile__("mrc	p10, 7, %0, c0, c0, 0" : "=r" (tmp));
			kprintf("CPU%d: VFP %x\n", cpunum, tmp);
		}
	}

	/*
	 * Set TTBCR to use 8K boundary for TTBR registers:
	 * 80000000-ffffffff uses TTBR0 - system (per-cpu) L1 table
	 * 00000000-7fffffff uses TTBR1 - per-process L1 table
	 */
	__asm__ __volatile__("mcr	p15, 0, %0, c2, c0, 2" : : "r" (1));

	/*
	 * Initialise TTBR1 to the cpu's system L1 table.
	 *
	 * The cpu-specific armv_setup_xxx() is expected to have set up any
	 * special TTBR bits in this value.
	 */
	__asm__ __volatile__("mcr	p15, 0, %0, c2, c0, 1" : : "r" (L1_paddr + (cpunum * ARM_L1_SIZE)));

	/*
	 * Indicate we are using ARMv6 extended page tables
	 */
	cpu->flags |= ARM_CPU_FLAG_V6 | ARM_CPU_FLAG_V6_ASID;

	/*
	 * Allow modulo-4 alignment checking instead of default modulo-8 check
	 */
	mmu_cr_set |= ARM_MMU_CR_U;
}


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/armv_setup_v6.c $ $Rev: 604842 $" );
