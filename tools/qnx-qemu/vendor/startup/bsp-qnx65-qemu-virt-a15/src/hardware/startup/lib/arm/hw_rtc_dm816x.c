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
#include <arm/dm816x.h>

/*
 * Clock setup for DM816X part.
 * addresses that we use (all fields are BCD)
 *
 * 0 seconds
 * 1 minutes
 * 2 hours
 * 3 day of week (we set this up but do not use it)
 * 4 day
 * 5 month
 * 6 year (0-99)
 * 7 century
 * f control b register
 */


unsigned long
rtc_time_dm816x(paddr_t base, unsigned reg_shift, int mmap, int cent_reg) {
    struct tm    tm;

    /*
     * Tell Neutrino what kind of chip for 'rtc' utility
     */
    hwi_add_rtc("dm816x", base, reg_shift, 32, mmap, 0x07);

    chip_access(base, reg_shift, mmap, 32);

    // read RTC time
    tm.tm_sec  = chip_read32(DM816X_RTC_SECONDS);
    tm.tm_min  = chip_read32(DM816X_RTC_MINUTES);
    tm.tm_hour = chip_read32(DM816X_RTC_HOURS);
    tm.tm_mday = chip_read32(DM816X_RTC_DAYS);
    tm.tm_mon  = chip_read32(DM816X_RTC_MONTHS);
    tm.tm_year = chip_read32(DM816X_RTC_YEARS);


    tm.tm_sec  = bcd2bin(tm.tm_sec);
    tm.tm_min  = bcd2bin(tm.tm_min);
    tm.tm_hour = bcd2bin(tm.tm_hour);
    tm.tm_mday = bcd2bin(tm.tm_mday);
    tm.tm_mon  = bcd2bin(tm.tm_mon);
    tm.tm_year = bcd2bin(tm.tm_year);

    chip_done();

    tm.tm_mon -= 1;
    if(tm.tm_year < 70) {
        tm.tm_year += 100; //21st century.
    }

    return(calc_time_t(&tm));
}

__SRCVERSION("hw_rtc_dm816x.c $Rev: 574390 $");
