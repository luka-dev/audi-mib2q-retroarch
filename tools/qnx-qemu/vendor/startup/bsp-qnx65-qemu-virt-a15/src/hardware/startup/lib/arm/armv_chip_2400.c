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

const struct armv_chip armv_chip_ixp2400 = {
	0x4190,									// cpuid
	"ixp2400",								// name
	ARM_MMU_CR_S|ARM_MMU_CR_I|ARM_MMU_CR_Z,	// mmu_cr_set
	0,										// mmu_cr_clr	FIXME
	2,										// cycles
	&armv_cache_xscale,						// cache
	&power_xscale,							// power
	&page_flush_xscale,						// flush
	&page_flush_deferred_xscale,			// deferred
	&armv_pte_ixp2xxx,						// pte
	&armv_pte_ixp2xxx,						// pte_wa
	0,										// pte_wb: not supported see armv_pte_ixp2xxx.c
	0,										// pte_wt: not supported see armv_pte_ixp2xxx.c
	armv_setup_xscale,						// setup
};


__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/lib/arm/armv_chip_2400.c $ $Rev: 604842 $" );
