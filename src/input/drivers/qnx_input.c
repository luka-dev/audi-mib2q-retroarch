/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* MHI2Q QNX 6.5 input — MS1 null stub.
 * The BlackBerry10 driver was screen/bps event based; MHI2Q routes buttons from
 * the HMI (DSI keypad) / /dev/input. That is milestone 4 — for now this is a
 * no-op driver so the frontend links and runs. */

#include <stdint.h>
#include <stdlib.h>

#include "../input_driver.h"

static void *qnx_input_init(const char *joypad_driver)
{
   /* non-NULL opaque handle so the frontend keeps the driver */
   return calloc(1, sizeof(int));
}

static void qnx_input_poll(void *data) { }

static int16_t qnx_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   return 0;
}

static void qnx_input_free_input(void *data)
{
   if (data)
      free(data);
}

static uint64_t qnx_input_get_capabilities(void *data)
{
   return (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_ANALOG);
}

input_driver_t input_qnx = {
   qnx_input_init,
   qnx_input_poll,
   qnx_input_state,
   qnx_input_free_input,
   NULL,
   NULL,
   qnx_input_get_capabilities,
   "qnx_input",
   NULL,
   NULL,
   NULL
};
