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

const struct armv_chip armv_chip_sa1100 = {
	0xa110,						// cpuid
	"sa1100",					// name
	ARM_MMU_CR_S|ARM_MMU_CR_I,	// mmu_cr
	0,							// mmu_cr_clr	FIXME
	3,							// cycles
	&armv_cache_sa1100,			// cache
	&power_sa1100,				// power
	&page_flush_sa,				// flush
	&page_flush_deferred_sa,	// deferred
	&armv_pte_v4wb,				// pte
	0,							// pte_wa: not supported by core
	&armv_pte_v4wb,				// pte_wb
	0,							// pte_wt: not supported by core
	0,							// setup
};


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/armv_chip_sa1100.c $ $Rev: 604842 $" );
