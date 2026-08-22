/*
 * $QNXLicenseC:
 * Copyright 2008-2009, QNX Software Systems.
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



#include "externs.h"

static inline int tx_interrupt(DEV_USART *dev)
{
	dev->tty.un.s.tx_tmr = 0;	/* clear Timeout */
	return tto(&dev->tty, TTO_DATA, 0);
}

static inline int rx_interrupt(DEV_USART *dev)
{
	int	 status = 0;
	uintptr_t	base = dev->base;
	unsigned    key = 0;
	while (!(in32(base + PL011_FR) & PL011_FR_RXFE ))
	{
		key = in32(base + PL011_DR);
		status |= tti(&dev->tty, key);
	}
	out32(base+ PL011_ICR, 0x7FF);
	return status;
}

static inline int do_interrupt(DEV_USART *dev, int id)
{
	int	sts;
	sts = rx_interrupt(dev);
	return sts;
}

/*
 * Serial interrupt handler
 */
static const struct sigevent * ser_intr(void *area, int id)
{
	DEV_USART	*dev = area;
	if (do_interrupt(dev, id) && (dev->tty.flags & EVENT_QUEUED) == 0) {
		dev_lock(&ttyctrl);
		ttyctrl.event_queue[ttyctrl.num_events++] = &dev->tty;
		atomic_set(&dev->tty.flags, EVENT_QUEUED);
		dev_unlock(&ttyctrl);
		return &ttyctrl.event;
	}

	return 0;
}

void
ser_attach_intr(DEV_USART *dev)
{
	uintptr_t	base = dev->base;
	dev->intr=PL011_IRQ;
	out32(base+PL011_IMSC, PL011_IMSC_CTSRM | PL011_IMSC_RXIM| PL011_IMSC_RTIM |
			                      (1<<7) | (1<<8) | (1<<9) | PL011_IMSC_OEIM);
	dev->iid = InterruptAttach(dev->intr, ser_intr, dev, 0, 0);
}

