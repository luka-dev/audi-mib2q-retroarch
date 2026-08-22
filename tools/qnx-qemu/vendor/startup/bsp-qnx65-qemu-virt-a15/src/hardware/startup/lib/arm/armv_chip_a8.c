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

const struct armv_chip armv_chip_a8 = {
	0xc080,								// cpuid
	"Cortex A8",						// name
	ARM_MMU_CR_I|ARM_MMU_CR_Z,			// mmu_cr
	0,									// mmu_cr_clr	FIXME
	2,									// cycles	FIXME
	&armv_cache_a8,						// cache
	&power_v7_wfi,						// power	
	&page_flush_a8,						// flush
	&page_flush_deferred_a8,			// deferred
	&armv_pte_v7wa,						// pte
	&armv_pte_v7wa,						// pte_wa
	&armv_pte_v7wb,						// pte_wb
	&armv_pte_v7wt,						// pte_wt
	armv_setup_a8,						// setup
};


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/armv_chip_a8.c $ $Rev: 604842 $" );
