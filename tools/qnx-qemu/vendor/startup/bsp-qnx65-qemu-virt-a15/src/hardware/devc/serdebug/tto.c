/*
 * Copyright 2008-2009, QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may not
 * reproduce, modify or distribute this software except in compliance with the
 * License. You may obtain a copy of the License at:
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis, WITHOUT
 * WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as contributors under
 * the License or as licensors under other terms.  Please review this entire
 * file for other proprietary rights or license notices, as well as the QNX
 * Development Suite License Guide at http://licensing.qnx.com/license-guide/
 * for other information.
 */



#include "externs.h"


int
tto(TTYDEV *ttydev, int action, int arg1)
{
	TTYBUF 		*bup = &ttydev->obuf;
	DEV_USART	*dev = (DEV_USART *)ttydev;
	uintptr_t	base = dev->base;
	uint8_t		c;

	switch (action) {
	case TTO_STTY:
		ser_stty(dev);
		return 0;
 
	case TTO_CTRL:
		return 0;

	case TTO_LINESTATUS:
		return in32(base + PL011_FR); ;

	case TTO_DATA:
		break;

	default:
		return 0;
	}
	while ((bup->cnt > 0 ))
		{
		while ( in32(base + PL011_FR) & PL011_FR_TXFF )//#FIX? 1<<7
		{}
			dev_lock(&dev->tty);
         	c = tto_getchar(&dev->tty);
			dev_unlock(&dev->tty);
			dev->tty.un.s.tx_tmr = 3;		/* Timeout 3 */
			out32(base + PL011_DR, c);
			/*
			 * Clear the OSW_PAGED_OVERRIDE flag as we only want
			 * one character to be transmitted in this case.
			 */
			if (dev->tty.xflags & OSW_PAGED_OVERRIDE)
			{
				atomic_clr(&dev->tty.xflags, OSW_PAGED_OVERRIDE);
				break;
		    }
	}

	/*
	 * If anyone is waiting to write, kick them when buffer drains to 1/4 full.
	 */
	return (tto_checkclients(&dev->tty));
}


/** Already initialized ,No plans to change the baud rate */
void ser_stty(DEV_USART *dev)
{
	uintptr_t	base;
	unsigned	mode = 0;

	/*
	 * Charactor length
	 */
	switch (dev->tty.c_cflag & CSIZE) {
	case CS5:
		mode |= 0 << 5; break;

	case CS6:
		mode |= 1 << 5; break;

	case CS7:
		mode |= 2 << 5; break;

	case CS8:
	default:
		mode |= 3 << 5; break;
	}

	/*
	 * 2 stop bit ?
	 */
	if (dev->tty.c_cflag & CSTOPB)
		mode |= 1 << 3;

	/*
	 * Parity
	 */
	if (dev->tty.c_cflag & PARENB) {
		if (dev->tty.c_cflag & PARODD)
			mode &= ~(1 << 2);
		else
			mode |= ~(1 << 2);
		if (dev->tty.c_cflag & PARSTK)
			mode |= ~(1 << 7);
		else
			mode &= ~(1 << 7);
	}
	else
		mode &= 1 << 1;

	if (mode == dev->mode && dev->tty.baud == dev->baud)
		return;

	dev->baud = dev->tty.baud;
	dev->mode = mode;

	base = dev->base;

	/* Reset UART */

	out16(base + PL011_CR, 0);
	out16(base+ PL011_ICR, 0x7FF);
	out16(base + PL011_IBRD, 1);
	out16(base + PL011_FBRD, 40);
	out16(base + PL011_LCR_H, PL011_LCR_H_WLEN8 | PL011_LCR_H_FEN);
	out16(base + PL011_IMSC, PL011_IMSC_CTSRM | PL011_IMSC_RTIM |
			                       (1 << 7) | (1 << 8) | (1 << 9) | PL011_IMSC_OEIM);
	out32(base + PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);

}

