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

const struct armv_chip armv_chip_1176 = {
	0xb760,										// cpuid
	"arm1176",									// name
	ARM_MMU_CR_XP|ARM_MMU_CR_I|ARM_MMU_CR_Z,	// mmu_cr_set
	ARM_MMU_CR_R|ARM_MMU_CR_S,					// mmu_cr_clr
	2,											// cycles (I/Z bit set)
	&armv_cache_1136,							// cache
	0,											// power	FIXME
	&page_flush_1136,							// flush
	&page_flush_deferred_1136,					// deferred
	&armv_pte_v6wb,								// pte
	0,											// pte_wa not supported
	&armv_pte_v6wb,								// pte_wb
	&armv_pte_v6wt,								// pte_wt
	armv_setup_1176,							// setup
};


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/armv_chip_1176.c $ $Rev: 604842 $" );
