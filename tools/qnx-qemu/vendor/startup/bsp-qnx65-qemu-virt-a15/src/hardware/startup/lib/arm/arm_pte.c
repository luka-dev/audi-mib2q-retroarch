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
 * The following pointer can be set by a board specific startup program
 * to override the value specified by the armv_chip for the CPU
 */
const struct armv_pte	*armv_pte;

void
arm_pte_setup()
{
	const struct armv_chip	*cfg;
	const struct armv_pte	*pte;
	struct arm_cpu_entry	*cpu = lsp.cpu.arm_cpu.p;

	if ((pte = armv_pte) == 0) {
		cfg = armv_chip_detect();
		if (cfg == 0) {
			crash("Unsupported CPUID");
		}
		if (cfg->pte == 0) {
			crash("No pte descriptors for %s", cfg->name);
		}

		/*
		 * Check if -w: overrides page table entries
		 */
		switch (arm_altpte) {
		case 0:		// No explicit -w options specified
					pte = cfg->pte;
					break;

		case 'a':	pte = cfg->pte_wa;
					break;

		case 'b':	pte = cfg->pte_wb;
					break;

		case 't':	pte = cfg->pte_wt;
					break;

		default:	pte = 0;
					break;
		}
		if (pte == 0) {
			/*
			 * Back to default if requested option is not implemented
			 */
			if (debug_flag) {
				kprintf("WARNING: ignoring unsupported -w pte option\n");
			}
			pte = cfg->pte;
		}
		armv_pte = pte;
	}

	/*
	 * Set the arm_cpu fields
	 */
	cpu->upte_ro = pte->upte_ro;
	cpu->upte_rw = pte->upte_rw;
	cpu->kpte_ro = pte->kpte_ro;
	cpu->kpte_rw = pte->kpte_rw;
	cpu->mask_nc = pte->mask_nc;
}


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/arm_pte.c $ $Rev: 604842 $" );
