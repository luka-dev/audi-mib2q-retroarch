/*
 * $QNXLicenseC:
 * Copyright 2009, QNX Software Systems.
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


/*
 * init_raminfo.c
 *	Tell syspage about our RAM configuration
 */
#include "startup.h"

extern int mtp, pin_mux, ddr_size;

void init_raminfo()
{
	/* MHI2Q: 256 MiB -> 1 GiB. Only works with the GENERIC SDP procnto-smp.
	 * The firmware procnto-smp (507904 bytes, EXEC) dies in bootimage_init@299
	 * for any value above 256 MiB — that cap lives in its closed page
	 * allocator and cannot be fixed from startup.
	 * The startup-2gb variant is this same tree built with MEG(2048); the qfs
	 * images must then be loaded above the enlarged pool. */
	add_ram(0x40000000, MEG(1024));
}

__SRCVERSION( "$URL: http://svn/product/tags/public/bsp/nto650/ti-omap3530-beagle/latest/src/hardware/startup/boards/omap3530/init_raminfo.c $ $Rev: 604842 $" );
