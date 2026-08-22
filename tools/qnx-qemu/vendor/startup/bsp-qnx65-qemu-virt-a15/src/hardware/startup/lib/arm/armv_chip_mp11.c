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

const struct armv_chip armv_chip_mp11 = {
	0xb020,													// cpuid
	"arm11 MPcore",											// name
	ARM_MMU_CR_XP|ARM_MMU_CR_I|ARM_MMU_CR_Z|ARM_MMU_CR_RR,	// mmu_cr
	ARM_MMU_CR_R|ARM_MMU_CR_S,								// mmu_cr_clr
	2,														// cycles (I/Z bit set)
	&armv_cache_mp11,										// cache
	&power_cp15_wfi,										// power
	&page_flush_mp11,										// flush
	&page_flush_deferred_mp11,								// deferred
	&armv_pte_v6mp,											// pte
	&armv_pte_v6mp,											// pte_wa
	0,														// pte_wb	FIXME
	0,														// pte_wt	FIXME
	armv_setup_mp11,										// setup
};


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/armv_chip_mp11.c $ $Rev: 604842 $" );
