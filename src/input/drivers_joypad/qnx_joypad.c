/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2026 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 */

/* QNX 6.5 HIDDI joypad backend.
 *
 * io-hid is the transport layer for both USB HID and Bluetooth HIDP. This
 * driver deliberately exposes stable *physical* controls to RetroArch:
 *
 *   HID buttons/axes/hats -> qnx joypad indices -> autoconfig -> RetroPad
 *
 * Device-specific semantic mappings therefore live in external .cfg files,
 * not here. Unknown devices receive RetroArch's generic QNX fallback profile
 * and can be corrected with "Bind All" / "Save Controller Profile".
 *
 * Two generic parsers are used:
 *   1. The public HIDDI extraction API (hidd_get_*).
 *   2. A generic HID report-descriptor parser, used when the QNX 6.5 HIDDI
 *      preparser does not expose values from an otherwise valid report. This
 *      is descriptor-driven.
 *
 * A small protocol layer complements those parsers for devices which are not
 * generic HID.  Known 8BitDo reports use the same physical ordering as the
 * external dinput-derived autoconfig profiles.  Xbox XUSB/GIP interfaces are
 * handled directly through io-usb/libusbdi in qnx_xusb.c; they are vendor
 * protocols and therefore never reach io-hid on many QNX installations.
 *
 * hidd_get_report_desc() is exported by QNX 6.5 libhiddi.so.1 and is used by
 * QNX's own HID clients, but was accidentally omitted from the public header.
 * We only use it after verifying the server reports HIDDI v1.00. If it is not
 * available, the complete public API path remains active.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rthreads/rthreads.h>
#include <sys/hiddi.h>
#include <sys/hidut.h>
#include <sys/usbdi.h>

#include <retro_miscellaneous.h>
#include <features/features_cpu.h>
#include <file/config_file.h>

#include "../input_driver.h"
#include "../../configuration.h"
#include "../../verbosity.h"
#include "../../tasks/tasks_internal.h"

#define QNX_MAX_PADS                MAX_USERS
#define QNX_MAX_REPORTS             32
#define QNX_MAX_REPORT_SCAN         32
#define QNX_MAX_COLLECTION_DEPTH    12
#define QNX_MAX_BUTTONS             256
#define QNX_MAX_AXES                8
#define QNX_MAX_HATS                4
#define QNX_MAX_BUTTON_USAGES       256
#define QNX_MAX_RAW_FIELDS          192
#define QNX_MAX_LOCAL_USAGES        32
#define QNX_MAX_GLOBAL_STACK        8
#define QNX_MAX_COLLECTION_STACK    16
#define QNX_MAX_OUTPUT_REPORT       128
#define QNX_INVALID_COLLECTION      0xff
#define QNX_INVALID_SLOT            0xff
#define QNX_INVALID_FIELD_INDEX     0xffff

enum qnx_pad_transport
{
   QNX_PAD_TRANSPORT_HIDDI = 0,
   QNX_PAD_TRANSPORT_XUSB
};

struct qnx_xusb_device;

/* Exported by the QNX 6.5 HIDDI v1.00 runtime but omitted from sys/hiddi.h. */
extern int hidd_get_report_desc(struct hidd_connection *connection,
      struct hidd_device_instance *device, uint8_t **descriptor,
      uint16_t *descriptor_len);

enum qnx_raw_field_kind
{
   QNX_RAW_FIELD_BUTTON = 0,
   QNX_RAW_FIELD_BUTTON_ARRAY,
   QNX_RAW_FIELD_AXIS,
   QNX_RAW_FIELD_HAT
};

struct qnx_raw_field
{
   uint32_t bit_offset;
   int32_t  logical_min;
   int32_t  logical_max;
   uint16_t usage_page;
   uint16_t usage;
   uint16_t usage_min;
   uint16_t usage_max;
   uint8_t  report_id;
   uint8_t  bit_size;
   uint8_t  collection;
   uint8_t  kind;
   uint16_t index;
};

struct qnx_raw_layout
{
   struct qnx_raw_field fields[QNX_MAX_RAW_FIELDS];
   uint16_t field_count;
   uint16_t report_bits[256];
   uint8_t  collection_count;
   bool     has_report_ids;
   bool     valid;
};

struct qnx_axis_desc
{
   int32_t logical_min;
   int32_t logical_max;
   int32_t neutral;
   uint8_t bit_size;
   bool    present;
   bool    neutral_valid;
};

struct qnx_hat_desc
{
   int32_t logical_min;
   int32_t logical_max;
   bool    present;
};

struct qnx_report_state
{
   struct hidd_collection      *collection;
   struct hidd_report_instance *instance;
   struct hidd_report          *handle;
   input_bits_t                 buttons;
   uint16_t                     report_len;
   uint16_t                     num_buttons;
   uint8_t                      report_id;
   uint8_t                      public_axis_mask;
   uint8_t                      public_hat_slot;
   uint8_t                      dump_count;
   uint8_t                      debug_change_count;
   uint32_t                     debug_last_signature;
   bool                         debug_signature_valid;
};

/* Device-specific output report layouts are data, not driver code.  Profiles
 * live beside RetroArch at rumble/qnx/<vid>_<pid>_<report-id>.cfg. */
struct qnx_rumble_profile
{
   uint8_t bytes[QNX_MAX_OUTPUT_REPORT];
   uint16_t minimum_len;
   uint8_t report_id;
   uint8_t strong_offset;
   uint8_t weak_offset;
   uint8_t sequence_offset;
   uint8_t sequence_shift;
   uint8_t sequence_modulo;
   uint8_t sequence;
   uint8_t crc32_seed;
   bool crc32_last4;
   bool valid;
};

struct qnx_output_state
{
   struct hidd_collection      *collection;
   struct hidd_report_instance *instance;
   struct hidd_report          *handle;
   struct qnx_rumble_profile    profile;
   uint16_t                     report_len;
};

struct qnx_pad
{
   struct hidd_device_instance *device;
   struct qnx_report_state      reports[QNX_MAX_REPORTS];
   struct qnx_output_state      output;
   struct qnx_raw_layout        raw_layout;
   struct qnx_axis_desc         axis_desc[QNX_MAX_AXES];
   struct qnx_hat_desc          hat_desc[QNX_MAX_HATS];
   input_bits_t                 buttons;
   int16_t                      axes[QNX_MAX_AXES];
   uint8_t                      hats[QNX_MAX_HATS];
   uint32_t                     devno;
   uint32_t                     generation;
   uint16_t                     vid;
   uint16_t                     pid;
   uint8_t                      report_count;
   uint8_t                      num_hats;
   uint8_t                      raw_collection;
   uint16_t                     rumble_strong;
   uint16_t                     rumble_weak;
   uint32_t                     protocol_init_mask;
   enum qnx_pad_transport       transport;
   struct qnx_xusb_device      *xusb;
   bool                         claimed;
   bool                         connected;
   bool                         warned_overflow;
   char                         name[128];
   char                         phys[64];
};

struct qnx_report_extra
{
   uint32_t generation;
   uint8_t  pad;
   uint8_t  report;
   uint8_t  reserved[2];
};

struct qnx_pad_snapshot
{
   input_bits_t buttons;
   int16_t      axes[QNX_MAX_AXES];
   uint8_t      hats[QNX_MAX_HATS];
   uint16_t     vid;
   uint16_t     pid;
   uint8_t      num_hats;
   bool         connected;
};

struct qnx_hid_global
{
   uint32_t usage_page;
   int32_t  logical_min;
   int32_t  logical_max;
   uint16_t report_size;
   uint16_t report_count;
   uint8_t  report_id;
};

struct qnx_hid_local
{
   uint32_t usages[QNX_MAX_LOCAL_USAGES];
   uint32_t usage_min;
   uint32_t usage_max;
   uint8_t  usage_count;
   bool     has_usage_min;
   bool     has_usage_max;
};

static struct qnx_pad          qnx_pads[QNX_MAX_PADS];
static struct hidd_connection *qnx_hidd_conn;
static slock_t                *qnx_hidd_lock;
static uint32_t                qnx_generation;
static bool                    qnx_debug;
static bool                    qnx_dump_reports;

static const uint16_t qnx_axis_usage[QNX_MAX_AXES] = {
   HIDD_USAGE_X, HIDD_USAGE_Y, HIDD_USAGE_Z, HIDD_USAGE_RX,
   HIDD_USAGE_RY, HIDD_USAGE_RZ, HIDD_USAGE_SLIDER, HIDD_USAGE_DIAL
};

static const uint8_t qnx_hat_lut[8] = {
   (1 << 0),
   (1 << 0) | (1 << 3),
   (1 << 3),
   (1 << 1) | (1 << 3),
   (1 << 1),
   (1 << 1) | (1 << 2),
   (1 << 2),
   (1 << 0) | (1 << 2)
};

/* ------------------------------------------------------ small primitives */

static bool qnx_env_enabled(const char *name)
{
   const char *value = getenv(name);
   return value && *value && strcmp(value, "0") != 0;
}

static int qnx_axis_index(uint16_t usage)
{
   int i;
   for (i = 0; i < QNX_MAX_AXES; i++)
      if (qnx_axis_usage[i] == usage)
         return i;
   return -1;
}

static uint32_t qnx_read_unsigned(const uint8_t *data, unsigned size)
{
   uint32_t value = 0;
   unsigned i;
   for (i = 0; i < size; i++)
      value |= (uint32_t)data[i] << (i * 8);
   return value;
}

static int32_t qnx_sign_extend(uint32_t value, unsigned bits)
{
   if (bits > 0 && bits < 32)
   {
      uint32_t sign = UINT32_C(1) << (bits - 1);
      if (value & sign)
         value |= ~((UINT32_C(1) << bits) - 1);
   }
   return (int32_t)value;
}

static bool qnx_read_bits(const uint8_t *data, uint32_t data_len,
      uint32_t bit_offset, unsigned bit_size, uint32_t *value)
{
   uint32_t out = 0;
   unsigned i;

   if (!data || !value || bit_size == 0 || bit_size > 32)
      return false;
   if ((uint64_t)bit_offset + bit_size > (uint64_t)data_len * 8)
      return false;

   for (i = 0; i < bit_size; i++)
   {
      uint32_t bit = bit_offset + i;
      if (data[bit >> 3] & (1 << (bit & 7)))
         out |= UINT32_C(1) << i;
   }
   *value = out;
   return true;
}

static uint16_t qnx_usage_page(uint32_t usage, uint32_t default_page)
{
   return (uint16_t)((usage > 0xffff) ? (usage >> 16) : default_page);
}

static uint16_t qnx_usage_id(uint32_t usage)
{
   return (uint16_t)(usage & 0xffff);
}

static void qnx_local_reset(struct qnx_hid_local *local)
{
   memset(local, 0, sizeof(*local));
}

static uint32_t qnx_local_usage_at(const struct qnx_hid_local *local,
      unsigned index)
{
   if (local->usage_count)
   {
      if (index < local->usage_count)
         return local->usages[index];
      return local->usages[local->usage_count - 1];
   }
   if (local->has_usage_min)
   {
      uint32_t usage = local->usage_min + index;
      if (local->has_usage_max && usage > local->usage_max)
         usage = local->usage_max;
      return usage;
   }
   return 0;
}

static int qnx_layout_hat_index(const struct qnx_raw_layout *layout,
      uint8_t collection)
{
   uint16_t i;
   int count = 0;
   for (i = 0; i < layout->field_count; i++)
      if (layout->fields[i].collection == collection &&
          layout->fields[i].kind == QNX_RAW_FIELD_HAT)
         count++;
   return count;
}

static void qnx_layout_add_field(struct qnx_raw_layout *layout,
      const struct qnx_hid_global *global,
      uint32_t bit_offset, uint32_t usage,
      uint32_t usage_min, uint32_t usage_max,
      uint8_t collection, bool is_array)
{
   struct qnx_raw_field *field;
   uint16_t page = qnx_usage_page(usage ? usage : usage_min,
         global->usage_page);
   uint16_t id = qnx_usage_id(usage);
   int axis = qnx_axis_index(id);
   int hat;

   if (layout->field_count >= QNX_MAX_RAW_FIELDS ||
       global->report_size == 0 || global->report_size > 32)
      return;

   if (is_array)
   {
      if (page != HIDD_PAGE_BUTTONS)
         return;
   }
   else if (page != HIDD_PAGE_BUTTONS &&
            !(page == HIDD_PAGE_DESKTOP &&
              (axis >= 0 || id == HIDD_USAGE_HAT_SWITCH)))
      return;

   field = &layout->fields[layout->field_count++];
   memset(field, 0, sizeof(*field));
   field->bit_offset    = bit_offset;
   field->logical_min   = global->logical_min;
   field->logical_max   = global->logical_max;
   field->usage_page    = page;
   field->usage         = id;
   field->usage_min     = qnx_usage_id(usage_min);
   field->usage_max     = qnx_usage_id(usage_max);
   field->report_id     = global->report_id;
   field->bit_size      = (uint8_t)global->report_size;
   field->collection    = collection;

   if (is_array)
   {
      field->kind  = QNX_RAW_FIELD_BUTTON_ARRAY;
      field->index = QNX_INVALID_FIELD_INDEX;
   }
   else if (page == HIDD_PAGE_BUTTONS)
   {
      field->kind  = QNX_RAW_FIELD_BUTTON;
      field->index = (id >= 1 && id <= QNX_MAX_BUTTONS)
         ? (uint16_t)(id - 1) : QNX_INVALID_FIELD_INDEX;
   }
   else if (id == HIDD_USAGE_HAT_SWITCH)
   {
      hat = qnx_layout_hat_index(layout, collection);
      field->kind  = QNX_RAW_FIELD_HAT;
      field->index = (hat >= 0 && hat < QNX_MAX_HATS)
         ? (uint16_t)hat : QNX_INVALID_FIELD_INDEX;
   }
   else
   {
      field->kind  = QNX_RAW_FIELD_AXIS;
      field->index = (axis >= 0) ? (uint16_t)axis : QNX_INVALID_FIELD_INDEX;
   }
}

/* Parse enough of the HID 1.11 report descriptor grammar to decode generic
 * gamepad buttons, arrays, axes and hats. All vendor-defined fields are
 * ignored. Bit offsets are maintained for every input report ID. */
static bool qnx_parse_report_descriptor(const uint8_t *descriptor,
      uint16_t descriptor_len, struct qnx_raw_layout *layout)
{
   struct qnx_hid_global global;
   struct qnx_hid_global global_stack[QNX_MAX_GLOBAL_STACK];
   struct qnx_hid_local local;
   int collection_stack[QNX_MAX_COLLECTION_STACK];
   uint32_t input_offsets[256];
   unsigned global_depth = 0;
   unsigned collection_depth = 0;
   int current_gamepad = -1;
   unsigned pos = 0;

   memset(layout, 0, sizeof(*layout));
   memset(&global, 0, sizeof(global));
   memset(input_offsets, 0, sizeof(input_offsets));
   qnx_local_reset(&local);

   while (pos < descriptor_len)
   {
      uint8_t prefix = descriptor[pos++];
      unsigned size;
      unsigned type;
      unsigned tag;
      uint32_t value;
      int32_t signed_value;

      if (prefix == 0xfe)
      {
         if (pos + 2 > descriptor_len)
            break;
         size = descriptor[pos];
         pos += 2;
         if (pos + size > descriptor_len)
            break;
         pos += size;
         continue;
      }

      size = prefix & 3;
      if (size == 3)
         size = 4;
      type = (prefix >> 2) & 3;
      tag  = (prefix >> 4) & 15;

      if (pos + size > descriptor_len)
         break;
      value        = qnx_read_unsigned(descriptor + pos, size);
      signed_value = qnx_sign_extend(value, size * 8);
      pos += size;

      if (type == 1) /* Global */
      {
         switch (tag)
         {
            case 0: global.usage_page  = value; break;
            case 1: global.logical_min = signed_value; break;
            case 2:
               global.logical_max = global.logical_min < 0
                  ? signed_value : (int32_t)value;
               break;
            case 7: global.report_size  = (uint16_t)value; break;
            case 8:
               global.report_id = (uint8_t)value;
               layout->has_report_ids = true;
               if (global.report_id && input_offsets[global.report_id] == 0)
                  input_offsets[global.report_id] = 8;
               break;
            case 9: global.report_count = (uint16_t)value; break;
            case 10:
               if (global_depth < QNX_MAX_GLOBAL_STACK)
                  global_stack[global_depth++] = global;
               break;
            case 11:
               if (global_depth)
                  global = global_stack[--global_depth];
               break;
            default:
               break;
         }
      }
      else if (type == 2) /* Local */
      {
         switch (tag)
         {
            case 0:
               if (local.usage_count < QNX_MAX_LOCAL_USAGES)
                  local.usages[local.usage_count++] = value;
               break;
            case 1:
               local.usage_min = value;
               local.has_usage_min = true;
               break;
            case 2:
               local.usage_max = value;
               local.has_usage_max = true;
               break;
            default:
               break;
         }
      }
      else if (type == 0) /* Main */
      {
         if (tag == 8) /* Input */
         {
            uint32_t offset = input_offsets[global.report_id];
            bool constant   = (value & 0x01) != 0;
            bool variable   = (value & 0x02) != 0;
            unsigned i;

            if (!constant && current_gamepad >= 0 &&
                current_gamepad < QNX_INVALID_COLLECTION)
            {
               for (i = 0; i < global.report_count; i++)
               {
                  uint32_t usage = qnx_local_usage_at(&local, i);
                  qnx_layout_add_field(layout, &global,
                        offset + i * global.report_size,
                        usage, local.usage_min, local.usage_max,
                        (uint8_t)current_gamepad, !variable);
               }
            }

            offset += (uint32_t)global.report_size * global.report_count;
            input_offsets[global.report_id] = offset;
            if (offset > layout->report_bits[global.report_id])
               layout->report_bits[global.report_id] = (uint16_t)
                  (offset > 0xffff ? 0xffff : offset);
         }
         else if (tag == 10) /* Collection */
         {
            uint32_t usage = qnx_local_usage_at(&local, 0);
            uint16_t page  = qnx_usage_page(usage, global.usage_page);
            uint16_t id    = qnx_usage_id(usage);

            if (collection_depth < QNX_MAX_COLLECTION_STACK)
               collection_stack[collection_depth++] = current_gamepad;

            if (value == HIDD_COLLECTION_TYPE_APPLICATION &&
                page == HIDD_PAGE_DESKTOP &&
                (id == HIDD_USAGE_GAMEPAD || id == HIDD_USAGE_JOYSTICK))
            {
               current_gamepad = layout->collection_count;
               if (layout->collection_count < QNX_INVALID_COLLECTION)
                  layout->collection_count++;
            }
         }
         else if (tag == 12) /* End Collection */
         {
            if (collection_depth)
               current_gamepad = collection_stack[--collection_depth];
            else
               current_gamepad = -1;
         }

         qnx_local_reset(&local);
      }
   }

   layout->valid = layout->field_count > 0;
   return layout->valid;
}

/* ------------------------------------------------------------ pad state */

static void qnx_pad_rebuild_buttons(struct qnx_pad *pad)
{
   unsigned r, b;
   BIT256_CLEAR_ALL(pad->buttons);
   for (r = 0; r < pad->report_count; r++)
      for (b = 0; b < QNX_MAX_BUTTONS; b++)
         if (BIT256_GET(pad->reports[r].buttons, b))
            BIT256_SET(pad->buttons, b);
}

static int32_t qnx_axis_raw_value(uint32_t raw, uint8_t bit_size,
      int32_t logical_min)
{
   if (logical_min < 0)
      return qnx_sign_extend(raw, bit_size);
   return (int32_t)raw;
}

/* The first sample is the neutral point for unsigned axes. This correctly
 * distinguishes centred sticks from unipolar triggers without guessing from
 * X/Y/Z/Rx naming. Signed axes use zero when it is inside the logical range. */
static int16_t qnx_axis_scale(struct qnx_axis_desc *desc, int32_t value)
{
   int64_t scaled;

   if (desc->logical_max <= desc->logical_min)
      return 0;

   if (!desc->neutral_valid)
   {
      desc->neutral = (desc->logical_min < 0 && desc->logical_max > 0)
         ? 0 : value;
      desc->neutral_valid = true;
      return 0;
   }

   if (value < desc->logical_min)
      value = desc->logical_min;
   else if (value > desc->logical_max)
      value = desc->logical_max;

   if (value >= desc->neutral)
   {
      int32_t span = desc->logical_max - desc->neutral;
      if (span <= 0)
         return 0;
      scaled = ((int64_t)value - desc->neutral) * 32767 / span;
   }
   else
   {
      int32_t span = desc->neutral - desc->logical_min;
      if (span <= 0)
         return 0;
      scaled = -((int64_t)desc->neutral - value) * 32767 / span;
   }

   if (scaled > 32767)
      scaled = 32767;
   else if (scaled < -32767)
      scaled = -32767;
   return (int16_t)scaled;
}

static uint8_t qnx_hat_value(int32_t value, int32_t logical_min,
      int32_t logical_max)
{
   int32_t direction;
   int32_t count;

   if (value < logical_min || value > logical_max)
      return 0;

   direction = value - logical_min;
   count     = logical_max - logical_min + 1;
   if (count == 4)
      direction *= 2;
   if (direction < 0 || direction >= 8)
      return 0;
   return qnx_hat_lut[direction];
}

/* Protocol details in this function are adapted from SDL's zlib-licensed
 * HIDAPI 8BitDo driver.  Indices intentionally remain physical DInput order,
 * because pkg/autoconfig/qnx is derived from Libretro's DInput profiles. */
static bool qnx_is_enhanced_8bitdo(uint16_t vid, uint16_t pid)
{
   if (vid != 0x2dc8)
      return false;
   switch (pid)
   {
      case 0x6000: case 0x6100: /* SF30 Pro USB/BT */
      case 0x6001: case 0x6101: /* SN30 Pro USB/BT */
      case 0x6003: case 0x6006: /* Pro 2 USB/BT */
      case 0x6009:              /* Pro 3 */
      case 0x6012:              /* Ultimate 2 Wireless */
      case 0x202f:              /* Ultimate 3 */
      case 0x301c:              /* Ultimate 2C Wireless, 2.4 GHz dongle */
         return true;
      default:
         return false;
   }
}

static int16_t qnx_8bitdo_axis(uint8_t value)
{
   if (value == 0x7f)
      return 0;
   if (value < 0x7f)
      return (int16_t)(((int32_t)value - 0x7f) * 32768 / 0x7f);
   return (int16_t)(((int32_t)value - 0x7f) * 32767 / 0x80);
}

static void qnx_8bitdo_buttons(struct qnx_report_state *report,
      uint8_t primary, uint8_t secondary, uint8_t extra)
{
   BIT256_CLEAR_ALL(report->buttons);

   /* Nintendo-labelled physical order used by the QNX/DInput profiles:
    * A, B, (2 unused), X, Y, (5 unused), L1, R1, L2, R2,
    * Select, Start, Home, L3, R3, then the extra rear buttons. */
   if (primary & 0x02) BIT256_SET(report->buttons, 0);  /* A / east */
   if (primary & 0x01) BIT256_SET(report->buttons, 1);  /* B / south */
   if (primary & 0x10) BIT256_SET(report->buttons, 3);  /* X / north */
   if (primary & 0x08) BIT256_SET(report->buttons, 4);  /* Y / west */
   if (primary & 0x40) BIT256_SET(report->buttons, 6);
   if (primary & 0x80) BIT256_SET(report->buttons, 7);
   if (secondary & 0x01) BIT256_SET(report->buttons, 8);
   if (secondary & 0x02) BIT256_SET(report->buttons, 9);
   if (secondary & 0x04) BIT256_SET(report->buttons, 10);
   if (secondary & 0x08) BIT256_SET(report->buttons, 11);
   if (secondary & 0x10) BIT256_SET(report->buttons, 12);
   if (secondary & 0x20) BIT256_SET(report->buttons, 13);
   if (secondary & 0x40) BIT256_SET(report->buttons, 14);
   if (primary & 0x20) BIT256_SET(report->buttons, 15);
   if (primary & 0x04) BIT256_SET(report->buttons, 16);
   if (extra & 0x01) BIT256_SET(report->buttons, 17);
   if (extra & 0x02) BIT256_SET(report->buttons, 18);
   if (secondary & 0x80) BIT256_SET(report->buttons, 19);
}

static bool qnx_decode_8bitdo_report(struct qnx_pad *pad,
      struct qnx_report_state *report, const uint8_t *data, uint32_t len)
{
   uint8_t primary;
   uint8_t secondary;
   uint8_t extra;
   uint8_t hat;
   uint32_t signature;

   if (!qnx_is_enhanced_8bitdo(pad->vid, pad->pid) || !data)
      return false;

   if (len == 9)
   {
      primary   = data[0];
      secondary = data[1];
      extra     = 0;
      hat       = data[2];
      qnx_8bitdo_buttons(report, primary, secondary, extra);
      pad->hats[0] = hat < 8 ? qnx_hat_lut[hat] : 0;
      pad->axes[0] = qnx_8bitdo_axis(data[3]);
      pad->axes[1] = qnx_8bitdo_axis(data[4]);
      pad->axes[2] = qnx_8bitdo_axis(data[5]);
      pad->axes[5] = qnx_8bitdo_axis(data[6]);
   }
   else if (len >= 11 && (data[0] == 0x01 || data[0] == 0x03 ||
                          data[0] == 0x04))
   {
      primary   = data[8];
      secondary = data[9];
      extra     = data[10];
      hat       = data[1];
      if (data[7]) secondary |= 0x01; /* analog L2 -> physical button */
      if (data[6]) secondary |= 0x02; /* analog R2 -> physical button */
      qnx_8bitdo_buttons(report, primary, secondary, extra);
      pad->hats[0] = hat < 8 ? qnx_hat_lut[hat] : 0;
      pad->axes[0] = qnx_8bitdo_axis(data[2]);
      pad->axes[1] = qnx_8bitdo_axis(data[3]);
      pad->axes[2] = qnx_8bitdo_axis(data[4]);
      pad->axes[5] = qnx_8bitdo_axis(data[5]);
   }
   else
      return false;

   pad->num_hats = 1;
   qnx_pad_rebuild_buttons(pad);

   /* The generic raw dump is deliberately capped during startup.  Keep a
    * separate, transition-only trace for controller bring-up so a real
    * button press is never hidden by the initial neutral reports. */
   signature = (uint32_t)primary
         | ((uint32_t)secondary << 8)
         | ((uint32_t)extra << 16)
         | ((uint32_t)hat << 24);
   if (qnx_debug && report->debug_signature_valid &&
       signature != report->debug_last_signature &&
       report->debug_change_count < 64)
   {
      RARCH_LOG("[QNX HID]: pad %u 8BitDo state buttons=%02x/%02x/%02x "
            "hat=%02x axes=%d,%d,%d,%d.\n",
            (unsigned)(pad - qnx_pads), primary, secondary, extra, hat,
            pad->axes[0], pad->axes[1], pad->axes[2], pad->axes[5]);
      report->debug_change_count++;
   }
   report->debug_last_signature = signature;
   report->debug_signature_valid = true;
   return true;
}

static void qnx_axis_register(struct qnx_pad *pad, unsigned axis,
      int32_t logical_min, int32_t logical_max, uint8_t bit_size)
{
   struct qnx_axis_desc *desc;
   if (axis >= QNX_MAX_AXES || logical_max <= logical_min)
      return;
   desc = &pad->axis_desc[axis];
   if (!desc->present)
   {
      desc->logical_min = logical_min;
      desc->logical_max = logical_max;
      desc->bit_size    = bit_size;
      desc->present     = true;
      if (logical_min < 0 && logical_max > 0)
      {
         desc->neutral       = 0;
         desc->neutral_valid = true;
      }
   }
}

static void qnx_hat_register(struct qnx_pad *pad, unsigned hat,
      int32_t logical_min, int32_t logical_max)
{
   if (hat >= QNX_MAX_HATS)
      return;
   pad->hat_desc[hat].logical_min = logical_min;
   pad->hat_desc[hat].logical_max = logical_max;
   pad->hat_desc[hat].present     = true;
   if (pad->num_hats <= hat)
      pad->num_hats = (uint8_t)(hat + 1);
}

static bool qnx_decode_raw_report(struct qnx_pad *pad,
      struct qnx_report_state *report, const uint8_t *data, uint32_t data_len,
      bool *buttons_decoded, uint8_t *axis_mask, uint8_t *hat_mask)
{
   const struct qnx_raw_layout *layout = &pad->raw_layout;
   input_bits_t raw_buttons;
   uint8_t report_id = layout->has_report_ids && data_len ? data[0] : 0;
   bool matched = false;
   bool has_buttons = false;
   uint16_t i;

   *buttons_decoded = false;
   *axis_mask       = 0;
   *hat_mask        = 0;
   BIT256_CLEAR_ALL(raw_buttons);

   if (!layout->valid)
      return false;

   for (i = 0; i < layout->field_count; i++)
   {
      const struct qnx_raw_field *field = &layout->fields[i];
      uint32_t raw;
      int32_t value;

      if (field->collection != pad->raw_collection ||
          field->report_id != report_id)
         continue;
      if (!qnx_read_bits(data, data_len, field->bit_offset,
            field->bit_size, &raw))
         continue;

      matched = true;
      value = qnx_axis_raw_value(raw, field->bit_size, field->logical_min);

      switch (field->kind)
      {
         case QNX_RAW_FIELD_BUTTON:
            has_buttons = true;
            if (field->index != QNX_INVALID_FIELD_INDEX && value)
               BIT256_SET(raw_buttons, field->index);
            break;
         case QNX_RAW_FIELD_BUTTON_ARRAY:
            has_buttons = true;
            if (value >= field->usage_min && value <= field->usage_max &&
                value >= 1 && value <= QNX_MAX_BUTTONS)
               BIT256_SET(raw_buttons, (unsigned)value - 1);
            break;
         case QNX_RAW_FIELD_AXIS:
            if (field->index < QNX_MAX_AXES)
            {
               qnx_axis_register(pad, field->index, field->logical_min,
                     field->logical_max, field->bit_size);
               pad->axes[field->index] = qnx_axis_scale(
                     &pad->axis_desc[field->index], value);
               *axis_mask |= 1 << field->index;
            }
            break;
         case QNX_RAW_FIELD_HAT:
            if (field->index < QNX_MAX_HATS)
            {
               qnx_hat_register(pad, field->index, field->logical_min,
                     field->logical_max);
               pad->hats[field->index] = qnx_hat_value(value,
                     field->logical_min, field->logical_max);
               *hat_mask |= 1 << field->index;
            }
            break;
      }
   }

   if (has_buttons)
   {
      report->buttons = raw_buttons;
      *buttons_decoded = true;
   }
   return matched;
}

static bool qnx_decode_public_buttons(struct qnx_report_state *report,
      void *report_data)
{
   hidd_button_t buttons[QNX_MAX_BUTTON_USAGES];
   uint16_t usages[QNX_MAX_BUTTON_USAGES];
   uint16_t count = QNX_MAX_BUTTON_USAGES;
   input_bits_t state;
   bool saw_button_page = false;
   unsigned i;

   BIT256_CLEAR_ALL(state);

   if (hidd_get_all_buttons(report->instance, report_data,
         buttons, &count) == EOK)
   {
      for (i = 0; i < count; i++)
         if (buttons[i].usage_page == HIDD_PAGE_BUTTONS)
         {
            saw_button_page = true;
            if (buttons[i].usage >= 1 && buttons[i].usage <= QNX_MAX_BUTTONS)
               BIT256_SET(state, buttons[i].usage - 1);
         }
      if (saw_button_page || count == 0)
      {
         report->buttons = state;
         return true;
      }
   }

   count = QNX_MAX_BUTTON_USAGES;
   if (hidd_get_buttons(report->instance, report->collection,
         HIDD_PAGE_BUTTONS, report_data, usages, &count) == EOK)
   {
      BIT256_CLEAR_ALL(state);
      for (i = 0; i < count; i++)
         if (usages[i] >= 1 && usages[i] <= QNX_MAX_BUTTONS)
            BIT256_SET(state, usages[i] - 1);
      report->buttons = state;
      return true;
   }

   return false;
}

static void qnx_decode_public_values(struct qnx_pad *pad,
      struct qnx_report_state *report, void *report_data,
      uint8_t raw_axis_mask, uint8_t raw_hat_mask)
{
   unsigned i;

   for (i = 0; i < QNX_MAX_AXES; i++)
      if ((report->public_axis_mask & (1 << i)) &&
          !(raw_axis_mask & (1 << i)))
      {
         uint32_t raw = 0;
         if (hidd_get_usage_value(report->instance, report->collection,
               HIDD_PAGE_DESKTOP, qnx_axis_usage[i], report_data, &raw) == EOK)
         {
            int32_t value = qnx_axis_raw_value(raw,
                  pad->axis_desc[i].bit_size,
                  pad->axis_desc[i].logical_min);
            pad->axes[i] = qnx_axis_scale(&pad->axis_desc[i], value);
         }
      }

   if (report->public_hat_slot < QNX_MAX_HATS &&
       !(raw_hat_mask & (1 << report->public_hat_slot)))
   {
      uint32_t raw = 0;
      unsigned hat = report->public_hat_slot;
      if (hidd_get_usage_value(report->instance, report->collection,
            HIDD_PAGE_DESKTOP, HIDD_USAGE_HAT_SWITCH,
            report_data, &raw) == EOK)
         pad->hats[hat] = qnx_hat_value((int32_t)raw,
               pad->hat_desc[hat].logical_min,
               pad->hat_desc[hat].logical_max);
   }
}

/* ------------------------------------------------------ output/rumble data */

static bool qnx_output_report_id(struct hidd_report_instance *instance,
      uint8_t *report_id)
{
   hidd_report_props_t *props;
   uint16_t count = 0;
   uint16_t len;
   bool result = false;

   if (!instance || !report_id ||
       hidd_get_num_props(instance, &count) != EOK || !count)
      return false;
   props = (hidd_report_props_t*)calloc(count, sizeof(*props));
   if (!props)
      return false;
   len = count;
   if (hidd_get_report_props(instance, props, &len) == EOK && len)
   {
      *report_id = props[0].report_id;
      result = true;
   }
   free(props);
   return result;
}

static void qnx_init_feature_report(struct hidd_connection *connection,
      hidd_device_instance_t *device, struct qnx_pad *pad,
      struct hidd_report_instance *instance)
{
   struct hidd_report *handle = NULL;
   uint8_t data[QNX_MAX_OUTPUT_REPORT];
   uint16_t report_len = 0;
   uint8_t report_id = 0;
   uint32_t init_bit = 0;
   bool send = false;
   int result;

   if (!instance ||
       !qnx_output_report_id(instance, &report_id) ||
       hidd_report_len(instance, &report_len) != EOK ||
       !report_len || report_len > sizeof(data))
      return;

   if (qnx_is_enhanced_8bitdo(pad->vid, pad->pid) &&
       (report_id == 0x06 || report_id == 0x30))
      init_bit = report_id == 0x06 ? (1u << 0) : (1u << 1);
   else if (pad->vid == 0x054c && pad->pid == 0x0268)
   {
      if (report_id == 0xf2) init_bit = 1u << 2;
      if (report_id == 0xf5) init_bit = 1u << 3;
      if (report_id == 0xf4)
      {
         init_bit = 1u << 4;
         send = true;
      }
   }

   if (!init_bit || (pad->protocol_init_mask & init_bit))
      return;
   if (hidd_report_attach(connection, device, instance, 0, 0,
         &handle) != EOK || !handle)
      return;

   memset(data, 0, report_len);
   data[0] = report_id;
   if (send)
   {
      /* DualShock 3 Bluetooth: select full operational report mode. */
      if (report_len >= 5)
      {
         data[1] = 0x42;
         data[2] = 0x03;
      }
      result = hidd_send_report(handle, data);
   }
   else
      result = hidd_get_report(handle, data);

   hidd_report_detach(handle);
   if (result == EOK)
   {
      pad->protocol_init_mask |= init_bit;
      RARCH_LOG("[QNX HID]: initialized feature report 0x%02x for %s.\n",
            report_id, pad->name);
   }
   else if (qnx_debug)
      RARCH_WARN("[QNX HID]: feature report 0x%02x failed for %s: %d.\n",
            report_id, pad->name, result);
}

static bool qnx_load_rumble_profile(uint16_t vid, uint16_t pid,
      uint8_t report_id, uint16_t report_len,
      struct qnx_rumble_profile *profile, char *path, size_t path_size)
{
   const char *data_dir = getenv("RA_DATA_DIR");
   config_file_t *config;
   unsigned value;
   unsigned i;
   char key[32];

   if (!profile || !path || path_size == 0 ||
       report_len == 0 || report_len > QNX_MAX_OUTPUT_REPORT)
      return false;
   if (!data_dir || !*data_dir)
      data_dir = "/mnt/app/root/retroarch";
   snprintf(path, path_size, "%s/rumble/qnx/%04x_%04x_%02x.cfg",
         data_dir, vid, pid, report_id);

   config = config_file_new(path);
   if (!config)
      return false;
   memset(profile, 0, sizeof(*profile));
   profile->report_id  = report_id;
   profile->minimum_len = 1;
   profile->bytes[0]   = report_id;

   if (config_get_uint(config, "report_id", &value) &&
       value != report_id)
      goto invalid;
   if (config_get_uint(config, "minimum_report_length", &value))
      profile->minimum_len = (uint16_t)value;
   if (!config_get_uint(config, "strong_offset", &value) || value > 255)
      goto invalid;
   profile->strong_offset = (uint8_t)value;
   if (!config_get_uint(config, "weak_offset", &value) || value > 255)
      goto invalid;
   profile->weak_offset = (uint8_t)value;

   if (config_get_uint(config, "sequence_offset", &value) && value <= 255)
      profile->sequence_offset = (uint8_t)value;
   if (config_get_uint(config, "sequence_shift", &value) && value <= 7)
      profile->sequence_shift = (uint8_t)value;
   if (config_get_uint(config, "sequence_modulo", &value) && value <= 255)
      profile->sequence_modulo = (uint8_t)value;
   if (config_get_uint(config, "crc32_seed", &value) && value <= 255)
   {
      profile->crc32_seed = (uint8_t)value;
      profile->crc32_last4 = true;
   }

   for (i = 0; i < report_len; i++)
   {
      snprintf(key, sizeof(key), "byte_%u", i);
      if (config_get_uint(config, key, &value))
      {
         if (value > 255)
            goto invalid;
         profile->bytes[i] = (uint8_t)value;
      }
   }

   if (profile->minimum_len > report_len ||
       profile->strong_offset >= report_len ||
       profile->weak_offset >= report_len ||
       (profile->sequence_modulo &&
        profile->sequence_offset >= report_len) ||
       (profile->crc32_last4 && report_len < 5))
      goto invalid;

   profile->valid = true;
   config_file_free(config);
   return true;

invalid:
   RARCH_WARN("[QNX HID]: invalid rumble profile %s (report len %u).\n",
         path, report_len);
   config_file_free(config);
   memset(profile, 0, sizeof(*profile));
   return false;
}

static uint32_t qnx_crc32_byte(uint32_t crc, uint8_t byte)
{
   unsigned bit;
   crc ^= byte;
   for (bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
   return crc;
}

static void qnx_rumble_apply_crc32(uint8_t *data, uint16_t len,
      uint8_t seed)
{
   uint32_t crc = UINT32_C(0xffffffff);
   uint16_t i;
   crc = qnx_crc32_byte(crc, seed);
   for (i = 0; i < len - 4; i++)
      crc = qnx_crc32_byte(crc, data[i]);
   crc = ~crc;
   data[len - 4] = (uint8_t)crc;
   data[len - 3] = (uint8_t)(crc >> 8);
   data[len - 2] = (uint8_t)(crc >> 16);
   data[len - 1] = (uint8_t)(crc >> 24);
}

static void qnx_attach_rumble_report(struct hidd_connection *connection,
      hidd_device_instance_t *device, struct qnx_pad *pad, unsigned pad_slot,
      struct hidd_collection *collection,
      struct hidd_report_instance *instance, unsigned report_index)
{
   struct hidd_report *handle = NULL;
   struct qnx_rumble_profile profile;
   uint16_t report_len = 0;
   uint8_t report_id = 0;
   char path[PATH_MAX_LENGTH];

   if (!instance || pad->output.handle ||
       !qnx_output_report_id(instance, &report_id) ||
       hidd_report_len(instance, &report_len) != EOK || !report_len ||
       !qnx_load_rumble_profile(pad->vid, pad->pid, report_id,
            report_len, &profile, path, sizeof(path)))
      return;

   if (hidd_report_attach(connection, device, instance, 0, 0, &handle) != EOK ||
       !handle)
   {
      RARCH_WARN("[QNX HID]: could not attach output report %u for %s.\n",
            report_id, pad->name);
      return;
   }

   pad->output.collection = collection;
   pad->output.instance   = instance;
   pad->output.handle     = handle;
   pad->output.profile    = profile;
   pad->output.report_len = report_len;
   RARCH_LOG("[QNX HID]: pad %u output report-index %u attached "
         "(id %u, len %u, profile %s).\n", pad_slot, report_index,
         report_id, report_len, path);
}

/* ------------------------------------------------ report/collection setup */

static bool qnx_report_already_attached(const struct qnx_pad *pad,
      struct hidd_report_instance *instance)
{
   unsigned i;
   for (i = 0; i < pad->report_count; i++)
      if (pad->reports[i].instance == instance)
         return true;
   return false;
}

static void qnx_configure_public_report(struct qnx_pad *pad,
      struct qnx_report_state *report)
{
   hidd_report_props_t *props = NULL;
   uint16_t count = 0;
   uint16_t len;
   unsigned p;

   report->public_hat_slot = QNX_INVALID_SLOT;
   hidd_report_len(report->instance, &report->report_len);
   hidd_num_buttons(report->instance, &report->num_buttons);

   if (hidd_get_num_props(report->instance, &count) != EOK || !count)
      return;
   props = (hidd_report_props_t*)calloc(count, sizeof(*props));
   if (!props)
      return;
   len = count;
   if (hidd_get_report_props(report->instance, props, &len) != EOK)
   {
      free(props);
      return;
   }

   for (p = 0; p < len; p++)
   {
      uint16_t usage;
      if (p == 0)
         report->report_id = props[p].report_id;
      if (props[p].usage_page != HIDD_PAGE_DESKTOP)
         continue;

      for (usage = props[p].usage_min;
           usage <= props[p].usage_max && usage != 0xffff; usage++)
      {
         int axis = qnx_axis_index(usage);
         if (axis >= 0)
         {
            report->public_axis_mask |= 1 << axis;
            qnx_axis_register(pad, (unsigned)axis,
                  props[p].logical_min, props[p].logical_max,
                  (uint8_t)props[p].report_size);
         }
         else if (usage == HIDD_USAGE_HAT_SWITCH &&
                  report->public_hat_slot == QNX_INVALID_SLOT &&
                  pad->num_hats < QNX_MAX_HATS)
         {
            report->public_hat_slot = pad->num_hats;
            qnx_hat_register(pad, pad->num_hats,
                  props[p].logical_min, props[p].logical_max);
         }
      }
   }
   free(props);
}

static bool qnx_attach_report(struct hidd_connection *connection,
      hidd_device_instance_t *device, struct qnx_pad *pad, unsigned pad_slot,
      struct hidd_collection *collection,
      struct hidd_report_instance *instance, unsigned report_index)
{
   struct qnx_report_state *report;
   struct qnx_report_extra *extra;
   struct hidd_report *handle = NULL;
   unsigned report_slot;

   if (!instance || pad->report_count >= QNX_MAX_REPORTS ||
       qnx_report_already_attached(pad, instance))
      return false;

   if (hidd_report_attach(connection, device, instance, 0,
         sizeof(struct qnx_report_extra), &handle) != EOK || !handle)
      return false;

   report_slot = pad->report_count++;
   report      = &pad->reports[report_slot];
   memset(report, 0, sizeof(*report));
   report->collection = collection;
   report->instance   = instance;
   report->handle     = handle;
   report->public_hat_slot = QNX_INVALID_SLOT;
   BIT256_CLEAR_ALL(report->buttons);
   qnx_configure_public_report(pad, report);

   extra = (struct qnx_report_extra*)hidd_report_extra(handle);
   if (extra)
   {
      memset(extra, 0, sizeof(*extra));
      extra->generation = pad->generation;
      extra->pad        = (uint8_t)pad_slot;
      extra->report     = (uint8_t)report_slot;
   }

   RARCH_LOG("[QNX HID]: pad %u report-index %u attached "
         "(id %u, len %u, buttons %u, axes 0x%02x).\n",
         pad_slot, report_index, report->report_id, report->report_len,
         report->num_buttons, report->public_axis_mask);
   return true;
}

static void qnx_attach_collection_tree(struct hidd_connection *connection,
      hidd_device_instance_t *device, struct qnx_pad *pad, unsigned pad_slot,
      struct hidd_collection *collection, unsigned depth)
{
   struct hidd_collection **children = NULL;
   uint16_t child_count = 0;
   unsigned i;

   if (!collection || depth > QNX_MAX_COLLECTION_DEPTH)
      return;

   /* Report indices are not guaranteed to be contiguous. The controller used
    * during MHI2Q bring-up has no index 0 and exposes its input at index 1. */
   for (i = 0; i < QNX_MAX_REPORT_SCAN; i++)
   {
      struct hidd_report_instance *instance = NULL;
      if (hidd_get_report_instance(collection, (uint16_t)i,
            HID_INPUT_REPORT, &instance) == EOK && instance)
         qnx_attach_report(connection, device, pad, pad_slot,
               collection, instance, i);

      instance = NULL;
      if (hidd_get_report_instance(collection, (uint16_t)i,
            HID_OUTPUT_REPORT, &instance) == EOK && instance)
         qnx_attach_rumble_report(connection, device, pad, pad_slot,
               collection, instance, i);

      instance = NULL;
      if (hidd_get_report_instance(collection, (uint16_t)i,
            HID_FEATURE_REPORT, &instance) == EOK && instance)
         qnx_init_feature_report(connection, device, pad, instance);
   }

   if (hidd_get_collections(NULL, collection, &children,
         &child_count) != EOK || !children)
      return;
   for (i = 0; i < child_count; i++)
      qnx_attach_collection_tree(connection, device, pad, pad_slot,
            children[i], depth + 1);
}

static int qnx_claim_pad(void)
{
   int i;
   slock_lock(qnx_hidd_lock);
   for (i = 0; i < QNX_MAX_PADS; i++)
      if (!qnx_pads[i].claimed)
      {
         memset(&qnx_pads[i], 0, sizeof(qnx_pads[i]));
         qnx_pads[i].claimed    = true;
         qnx_pads[i].generation = ++qnx_generation;
         slock_unlock(qnx_hidd_lock);
         return i;
      }
   slock_unlock(qnx_hidd_lock);
   return -1;
}

static void qnx_release_unconnected_pad(unsigned slot)
{
   struct qnx_pad *pad;
   unsigned i;
   if (slot >= QNX_MAX_PADS)
      return;
   pad = &qnx_pads[slot];
   for (i = 0; i < pad->report_count; i++)
      if (pad->reports[i].handle)
         hidd_report_detach(pad->reports[i].handle);
   if (pad->output.handle)
      hidd_report_detach(pad->output.handle);
   slock_lock(qnx_hidd_lock);
   memset(pad, 0, sizeof(*pad));
   slock_unlock(qnx_hidd_lock);
}

static bool qnx_get_raw_layout(struct hidd_connection *connection,
      hidd_device_instance_t *device, struct qnx_raw_layout *layout)
{
   hidd_server_info_t info;
   uint8_t *descriptor = NULL;
   uint16_t descriptor_len = 0;
   bool result = false;

   memset(layout, 0, sizeof(*layout));
   memset(&info, 0, sizeof(info));
   if (hidd_server_info(connection, &info) != EOK ||
       info.vhid != HIDD_VERSION || info.vhidd != HIDD_VERSION)
      return false;

   if (hidd_get_report_desc(connection, device,
         &descriptor, &descriptor_len) == EOK &&
       descriptor && descriptor_len)
      result = qnx_parse_report_descriptor(descriptor,
            descriptor_len, layout);

   if (qnx_debug)
      RARCH_LOG("[QNX HID]: descriptor len %u, parsed fields %u, "
            "gamepad collections %u.\n", descriptor_len,
            layout->field_count, layout->collection_count);
   free(descriptor);
   return result;
}

/* Vendor-specific Xbox transports live in a separate source unit to keep the
 * generic HID parser readable. qnx_joypad.c is compiled both normally and by
 * the RetroArch unity/griffin build, so inclusion here is intentional. */
#include "qnx_xusb.c"

/* ---------------------------------------------------------- HID callbacks */

static void qnx_hidd_insertion(struct hidd_connection *connection,
      hidd_device_instance_t *device)
{
   struct hidd_collection **collections = NULL;
   struct qnx_raw_layout layout;
   uint16_t collection_count = 0;
   uint8_t raw_collection = 0;
   unsigned i;
   char product[128];

   if (!device || hidd_get_collections(device, NULL, &collections,
         &collection_count) != EOK || !collections)
      return;

   memset(product, 0, sizeof(product));
   if (hidd_get_product_string(connection, device,
         product, sizeof(product) - 1) != EOK)
      product[0] = '\0';
   product[sizeof(product) - 1] = '\0';

   qnx_get_raw_layout(connection, device, &layout);

   for (i = 0; i < collection_count; i++)
   {
      uint16_t page = 0;
      uint16_t usage = 0;
      struct qnx_pad *pad;
      int slot;

      if (hidd_collection_usage(collections[i], &page, &usage) != EOK ||
          page != HIDD_PAGE_DESKTOP ||
          (usage != HIDD_USAGE_GAMEPAD && usage != HIDD_USAGE_JOYSTICK))
         continue;

      slot = qnx_claim_pad();
      if (slot < 0)
      {
         RARCH_WARN("[QNX HID]: no free pad slot (max %d).\n",
               QNX_MAX_PADS);
         return;
      }

      pad                 = &qnx_pads[slot];
      pad->device         = device;
      pad->devno          = device->devno;
      pad->vid            = (uint16_t)device->device_ident.vendor_id;
      pad->pid            = (uint16_t)device->device_ident.product_id;
      pad->raw_collection = raw_collection++;
      pad->raw_layout     = layout;
      snprintf(pad->phys, sizeof(pad->phys), "io-hid/dev%u/col%u",
            (unsigned)pad->devno, (unsigned)pad->raw_collection);

      if (product[0])
         strlcpy(pad->name, product, sizeof(pad->name));
      else
         snprintf(pad->name, sizeof(pad->name), "HID %04x:%04x",
               pad->vid, pad->pid);

      qnx_attach_collection_tree(connection, device, pad, slot,
            collections[i], 0);

      if (!pad->report_count)
      {
         RARCH_WARN("[QNX HID]: %s collection has no attachable input "
               "reports.\n", pad->name);
         qnx_release_unconnected_pad((unsigned)slot);
         continue;
      }

      slock_lock(qnx_hidd_lock);
      pad->connected = true;
      slock_unlock(qnx_hidd_lock);

      RARCH_LOG("[QNX HID]: pad %d connected: %s (%04x:%04x, devno %u, "
            "%u reports, %s).\n", slot, pad->name, pad->vid, pad->pid,
            (unsigned)pad->devno, pad->report_count,
            usage == HIDD_USAGE_GAMEPAD ? "gamepad" : "joystick");

      input_autoconfigure_connect(pad->name, NULL, pad->phys, "qnx",
            (unsigned)slot, pad->vid, pad->pid);
   }
}

static void qnx_hidd_removal(struct hidd_connection *connection,
      hidd_device_instance_t *device)
{
   unsigned disconnected[QNX_MAX_PADS];
   char names[QNX_MAX_PADS][128];
   unsigned count = 0;
   unsigned i;

   if (!device)
      return;

   slock_lock(qnx_hidd_lock);
   for (i = 0; i < QNX_MAX_PADS; i++)
      if (qnx_pads[i].claimed && qnx_pads[i].device == device)
      {
         disconnected[count] = i;
         strlcpy(names[count], qnx_pads[i].name, sizeof(names[count]));
         count++;
         memset(&qnx_pads[i], 0, sizeof(qnx_pads[i]));
      }
   slock_unlock(qnx_hidd_lock);

   hidd_reports_detach(connection, device);

   for (i = 0; i < count; i++)
   {
      RARCH_LOG("[QNX HID]: pad %u disconnected (%s).\n",
            disconnected[i], names[i]);
      input_autoconfigure_disconnect(disconnected[i], "qnx");
   }
}

static void qnx_hidd_report(struct hidd_connection *connection,
      struct hidd_report *handle, void *report_data,
      uint32_t report_len, uint32_t flags, void *user)
{
   struct qnx_report_extra *extra;
   struct qnx_report_state *report;
   struct qnx_pad *pad;
   bool raw_matched;
   bool buttons_decoded;
   uint8_t raw_axis_mask;
   uint8_t raw_hat_mask;

   (void)connection;
   (void)user;

   if (!handle || !report_data || !qnx_hidd_lock)
      return;
   extra = (struct qnx_report_extra*)hidd_report_extra(handle);
   if (!extra || extra->pad >= QNX_MAX_PADS ||
       extra->report >= QNX_MAX_REPORTS)
      return;

   slock_lock(qnx_hidd_lock);
   pad = &qnx_pads[extra->pad];
   if (!pad->connected || pad->generation != extra->generation ||
       extra->report >= pad->report_count)
   {
      slock_unlock(qnx_hidd_lock);
      return;
   }
   report = &pad->reports[extra->report];
   if (report->handle != handle)
   {
      slock_unlock(qnx_hidd_lock);
      return;
   }

   if ((flags & HIDD_REPORT_BUFFER_OVERFLOW) && !pad->warned_overflow)
   {
      pad->warned_overflow = true;
      RARCH_WARN("[QNX HID]: report buffer overflow for %s.\n", pad->name);
   }

   if (qnx_dump_reports && report->dump_count < 8)
   {
      const uint8_t *bytes = (const uint8_t*)report_data;
      uint32_t i;
      uint32_t n = report_len < 32 ? report_len : 32;
      char line[3 * 32 + 1];
      size_t pos = 0;
      for (i = 0; i < n && pos + 3 < sizeof(line); i++)
         pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
               "%02x%s", bytes[i], i + 1 == n ? "" : " ");
      RARCH_LOG("[QNX HID]: pad %u report %u raw[%u]: %s\n",
            extra->pad, extra->report, (unsigned)report_len, line);
      report->dump_count++;
   }

   if (qnx_decode_8bitdo_report(pad, report,
         (const uint8_t*)report_data, report_len))
   {
      slock_unlock(qnx_hidd_lock);
      return;
   }

   raw_matched = qnx_decode_raw_report(pad, report,
         (const uint8_t*)report_data, report_len,
         &buttons_decoded, &raw_axis_mask, &raw_hat_mask);

   if (!buttons_decoded && report->num_buttons)
      buttons_decoded = qnx_decode_public_buttons(report, report_data);

   qnx_decode_public_values(pad, report, report_data,
         raw_axis_mask, raw_hat_mask);

   if (buttons_decoded)
      qnx_pad_rebuild_buttons(pad);

   if (qnx_debug && !raw_matched && !report->num_buttons &&
       !report->public_axis_mask &&
       report->public_hat_slot == QNX_INVALID_SLOT &&
       report->dump_count == 0)
      RARCH_DBG("[QNX HID]: ignored report without generic controls.\n");

   slock_unlock(qnx_hidd_lock);
}

static void qnx_hidd_event(struct hidd_connection *connection,
      hidd_device_instance_t *device, uint16_t type)
{
   (void)connection;
   (void)device;
   if (qnx_debug)
      RARCH_DBG("[QNX HID]: server event %u.\n", (unsigned)type);
}

/* ----------------------------------------------- RetroArch joypad interface */

static void *qnx_joypad_init(void *data)
{
   hidd_connect_parm_t parameters;
   static hidd_funcs_t functions;
   bool hidd_ok = false;
   bool xusb_ok = false;

   (void)data;
   qnx_debug        = qnx_env_enabled("RA_QNX_HID_DEBUG");
   qnx_dump_reports = qnx_env_enabled("RA_QNX_HID_DUMP");
   qnx_generation   = 0;
   memset(qnx_pads, 0, sizeof(qnx_pads));

   qnx_hidd_lock = slock_new();
   if (!qnx_hidd_lock)
   {
      RARCH_ERR("[QNX HID]: could not allocate state lock.\n");
      return NULL;
   }

   memset(&functions, 0, sizeof(functions));
   functions.nentries  = _HIDDI_NFUNCS;
   functions.insertion = qnx_hidd_insertion;
   functions.removal   = qnx_hidd_removal;
   functions.report    = qnx_hidd_report;
   functions.event     = qnx_hidd_event;

   memset(&parameters, 0, sizeof(parameters));
   parameters.path         = NULL;
   parameters.vhid         = HIDD_VERSION;
   parameters.vhidd        = HIDD_VERSION;
   parameters.evtbufsz     = 4096;
   parameters.device_ident = NULL;
   parameters.funcs        = &functions;
   parameters.connect_wait = HIDD_CONNECT_WAIT;

   if (hidd_connect(&parameters, &qnx_hidd_conn) != EOK)
   {
      RARCH_WARN("[QNX HID]: hidd_connect failed; is io-hid running?\n");
      qnx_hidd_conn = NULL;
   }
   else
   {
      hidd_ok = true;
      RARCH_LOG("[QNX HID]: connected to io-hid (USB + Bluetooth HIDP).\n");
   }

   xusb_ok = qnx_xusb_init();
   if (!hidd_ok && !xusb_ok)
   {
      slock_free(qnx_hidd_lock);
      qnx_hidd_lock = NULL;
      return NULL;
   }
   return (void*)-1;
}

static bool qnx_joypad_set_rumble(unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   struct qnx_pad *pad;
   struct qnx_rumble_profile *profile;
   uint8_t report[QNX_MAX_OUTPUT_REPORT];
   uint16_t new_strong;
   uint16_t new_weak;
   int result;

   if (!qnx_hidd_lock || port >= QNX_MAX_PADS)
      return false;

   slock_lock(qnx_hidd_lock);
   pad = &qnx_pads[port];
   if (!pad->connected)
   {
      slock_unlock(qnx_hidd_lock);
      return false;
   }
   if (pad->transport == QNX_PAD_TRANSPORT_XUSB)
   {
      bool result = qnx_xusb_set_rumble(pad, effect, strength);
      slock_unlock(qnx_hidd_lock);
      return result;
   }
   if (!pad->output.handle ||
       !pad->output.profile.valid ||
       pad->output.report_len > sizeof(report))
   {
      slock_unlock(qnx_hidd_lock);
      return false;
   }

   new_strong = pad->rumble_strong;
   new_weak   = pad->rumble_weak;
   if (effect == RETRO_RUMBLE_STRONG)
      new_strong = strength;
   else if (effect == RETRO_RUMBLE_WEAK)
      new_weak = strength;
   else
   {
      slock_unlock(qnx_hidd_lock);
      return false;
   }

   if (new_strong == pad->rumble_strong && new_weak == pad->rumble_weak)
   {
      slock_unlock(qnx_hidd_lock);
      return true;
   }

   profile = &pad->output.profile;
   memcpy(report, profile->bytes, pad->output.report_len);
   report[profile->strong_offset] = (uint8_t)
         (((uint32_t)new_strong + 128) / 257);
   report[profile->weak_offset] = (uint8_t)
         (((uint32_t)new_weak + 128) / 257);

   if (profile->sequence_modulo)
      report[profile->sequence_offset] |= (uint8_t)
            ((profile->sequence % profile->sequence_modulo)
             << profile->sequence_shift);
   if (profile->crc32_last4)
      qnx_rumble_apply_crc32(report, pad->output.report_len,
            profile->crc32_seed);

   result = hidd_send_report(pad->output.handle, report);
   if (result == EOK)
   {
      pad->rumble_strong = new_strong;
      pad->rumble_weak   = new_weak;
      if (profile->sequence_modulo)
         profile->sequence = (uint8_t)
               ((profile->sequence + 1) % profile->sequence_modulo);
   }
   else
      RARCH_WARN("[QNX HID]: rumble output failed for pad %u: %d.\n",
            port, result);
   slock_unlock(qnx_hidd_lock);
   return result == EOK;
}

static void qnx_joypad_destroy(void)
{
   unsigned i;
   for (i = 0; i < QNX_MAX_PADS; i++)
   {
      qnx_joypad_set_rumble(i, RETRO_RUMBLE_STRONG, 0);
      qnx_joypad_set_rumble(i, RETRO_RUMBLE_WEAK, 0);
   }
   qnx_xusb_destroy();
   if (qnx_hidd_conn)
      hidd_disconnect(qnx_hidd_conn);
   qnx_hidd_conn = NULL;

   if (qnx_hidd_lock)
   {
      slock_lock(qnx_hidd_lock);
      memset(qnx_pads, 0, sizeof(qnx_pads));
      slock_unlock(qnx_hidd_lock);
      slock_free(qnx_hidd_lock);
      qnx_hidd_lock = NULL;
   }
}

static void qnx_pad_snapshot(unsigned port, struct qnx_pad_snapshot *snapshot)
{
   memset(snapshot, 0, sizeof(*snapshot));
   if (!qnx_hidd_lock || port >= QNX_MAX_PADS)
      return;
   slock_lock(qnx_hidd_lock);
   if (qnx_pads[port].connected)
   {
      snapshot->buttons   = qnx_pads[port].buttons;
      memcpy(snapshot->axes, qnx_pads[port].axes, sizeof(snapshot->axes));
      memcpy(snapshot->hats, qnx_pads[port].hats, sizeof(snapshot->hats));
      snapshot->vid      = qnx_pads[port].vid;
      snapshot->pid      = qnx_pads[port].pid;
      snapshot->num_hats = qnx_pads[port].num_hats;
      snapshot->connected = true;
   }
   slock_unlock(qnx_hidd_lock);
}

static bool qnx_joypad_query_pad(unsigned port)
{
   bool connected = false;
   if (!qnx_hidd_lock || port >= QNX_MAX_PADS)
      return false;
   slock_lock(qnx_hidd_lock);
   connected = qnx_pads[port].connected;
   slock_unlock(qnx_hidd_lock);
   return connected;
}

static const char *qnx_joypad_name(unsigned port)
{
   if (port >= QNX_MAX_PADS || !qnx_pads[port].connected)
      return NULL;
   return qnx_pads[port].name;
}

static int32_t qnx_snapshot_button(const struct qnx_pad_snapshot *snapshot,
      uint16_t joykey)
{
   unsigned hat_dir = GET_HAT_DIR(joykey);
   if (!snapshot->connected)
      return 0;
   if (hat_dir)
   {
      unsigned hat = GET_HAT(joykey);
      if (hat >= snapshot->num_hats)
         return 0;
      switch (hat_dir)
      {
         case HAT_UP_MASK:    return !!(snapshot->hats[hat] & (1 << 0));
         case HAT_DOWN_MASK:  return !!(snapshot->hats[hat] & (1 << 1));
         case HAT_LEFT_MASK:  return !!(snapshot->hats[hat] & (1 << 2));
         case HAT_RIGHT_MASK: return !!(snapshot->hats[hat] & (1 << 3));
         default:             return 0;
      }
   }
   if (joykey < QNX_MAX_BUTTONS)
      return BIT256_GET(snapshot->buttons, joykey) ? 1 : 0;
   return 0;
}

static int32_t qnx_joypad_button(unsigned port, uint16_t joykey)
{
   struct qnx_pad_snapshot snapshot;
   qnx_pad_snapshot(port, &snapshot);
   return qnx_snapshot_button(&snapshot, joykey);
}

static void qnx_joypad_get_buttons(unsigned port, input_bits_t *state)
{
   struct qnx_pad_snapshot snapshot;
   if (!state)
      return;
   qnx_pad_snapshot(port, &snapshot);
   if (snapshot.connected)
      *state = snapshot.buttons;
   else
      BIT256_CLEAR_ALL_PTR(state);
}

static int16_t qnx_snapshot_axis(const struct qnx_pad_snapshot *snapshot,
      uint32_t joyaxis)
{
   if (AXIS_NEG_GET(joyaxis) < QNX_MAX_AXES)
   {
      int16_t value = snapshot->axes[AXIS_NEG_GET(joyaxis)];
      return value < 0 ? value : 0;
   }
   if (AXIS_POS_GET(joyaxis) < QNX_MAX_AXES)
   {
      int16_t value = snapshot->axes[AXIS_POS_GET(joyaxis)];
      return value > 0 ? value : 0;
   }
   return 0;
}

static int16_t qnx_joypad_axis(unsigned port, uint32_t joyaxis)
{
   struct qnx_pad_snapshot snapshot;
   qnx_pad_snapshot(port, &snapshot);
   return snapshot.connected ? qnx_snapshot_axis(&snapshot, joyaxis) : 0;
}

/* Known enhanced 8BitDo protocols have a stable physical layout. Normally
 * the external autoconfig profile translates this layout to RetroPad. Keep a
 * device-scoped fallback for constrained frontends where the asynchronous
 * autoconfig task has completed but its binds are not visible to the input
 * sampling path. This also guarantees that the menu remains recoverable. */
static int16_t qnx_8bitdo_fallback_state(
      const struct qnx_pad_snapshot *snapshot, int32_t threshold)
{
   int16_t result = 0;
   uint8_t hat;

#define QNX_8BITDO_MAP_BUTTON(physical, retro) \
   if (qnx_snapshot_button(snapshot, physical)) \
      result |= (int16_t)(1u << (retro))

   QNX_8BITDO_MAP_BUTTON(0,  RETRO_DEVICE_ID_JOYPAD_B);
   QNX_8BITDO_MAP_BUTTON(1,  RETRO_DEVICE_ID_JOYPAD_A);
   QNX_8BITDO_MAP_BUTTON(3,  RETRO_DEVICE_ID_JOYPAD_Y);
   QNX_8BITDO_MAP_BUTTON(4,  RETRO_DEVICE_ID_JOYPAD_X);
   QNX_8BITDO_MAP_BUTTON(6,  RETRO_DEVICE_ID_JOYPAD_L);
   QNX_8BITDO_MAP_BUTTON(7,  RETRO_DEVICE_ID_JOYPAD_R);
   QNX_8BITDO_MAP_BUTTON(8,  RETRO_DEVICE_ID_JOYPAD_L2);
   QNX_8BITDO_MAP_BUTTON(9,  RETRO_DEVICE_ID_JOYPAD_R2);
   QNX_8BITDO_MAP_BUTTON(10, RETRO_DEVICE_ID_JOYPAD_SELECT);
   QNX_8BITDO_MAP_BUTTON(11, RETRO_DEVICE_ID_JOYPAD_START);
   QNX_8BITDO_MAP_BUTTON(13, RETRO_DEVICE_ID_JOYPAD_L3);
   QNX_8BITDO_MAP_BUTTON(14, RETRO_DEVICE_ID_JOYPAD_R3);
#undef QNX_8BITDO_MAP_BUTTON

   hat = snapshot->num_hats ? snapshot->hats[0] : 0;
   if (hat & (1 << 0)) result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_UP);
   if (hat & (1 << 1)) result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
   if (hat & (1 << 2)) result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
   if (hat & (1 << 3)) result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);

   /* Ozone expects sticks to navigate even before a core is loaded. */
   if (snapshot->axes[0] < -threshold)
      result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
   else if (snapshot->axes[0] > threshold)
      result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
   if (snapshot->axes[1] < -threshold)
      result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_UP);
   else if (snapshot->axes[1] > threshold)
      result |= (int16_t)(1u << RETRO_DEVICE_ID_JOYPAD_DOWN);

   return result;
}

static int16_t qnx_joypad_state(rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds, unsigned port)
{
   static int16_t debug_last_state[QNX_MAX_PADS];
   static bool debug_state_valid[QNX_MAX_PADS];
   static bool debug_first_call;
   struct qnx_pad_snapshot snapshot;
   uint16_t port_index = joypad_info->joy_idx;
   int32_t threshold = (int32_t)(joypad_info->axis_threshold * 32768.0f);
   int16_t result = 0;
   unsigned i;

   (void)port;
   if (port_index >= QNX_MAX_PADS)
      return 0;
   qnx_pad_snapshot(port_index, &snapshot);
   if (!snapshot.connected)
      return 0;

   if (qnx_debug && !debug_first_call)
   {
      RARCH_LOG("[QNX Input]: state sampling active (frontend port %u, "
            "joypad index %u, %04x:%04x).\n", port,
            (unsigned)port_index, snapshot.vid, snapshot.pid);
      debug_first_call = true;
   }

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
   {
      const uint64_t joykey = binds[i].joykey != NO_BTN
         ? binds[i].joykey : joypad_info->auto_binds[i].joykey;
      const uint32_t joyaxis = binds[i].joyaxis != AXIS_NONE
         ? binds[i].joyaxis : joypad_info->auto_binds[i].joyaxis;

      if ((uint16_t)joykey != NO_BTN &&
          qnx_snapshot_button(&snapshot, (uint16_t)joykey))
         result |= 1 << i;
      else if (joyaxis != AXIS_NONE &&
               abs((int)qnx_snapshot_axis(&snapshot, joyaxis)) > threshold)
         result |= 1 << i;
   }

   if (!result && qnx_is_enhanced_8bitdo(snapshot.vid, snapshot.pid))
   {
      result = qnx_8bitdo_fallback_state(&snapshot, threshold);
      if (qnx_debug && result)
         RARCH_LOG("[QNX Input]: enhanced 8BitDo fallback mask=0x%04x.\n",
               (unsigned)(uint16_t)result);
   }

   if (qnx_debug && debug_state_valid[port_index] &&
       debug_last_state[port_index] != result)
      RARCH_LOG("[QNX Input]: pad %u RetroPad mask=0x%04x.\n",
            (unsigned)port_index, (unsigned)(uint16_t)result);
   debug_last_state[port_index]  = result;
   debug_state_valid[port_index] = true;
   return result;
}

/* HIDDI and USB input reports are asynchronous. Poll only advances the GIP
 * startup fallback for controllers which do not send an announce packet. */
static void qnx_joypad_poll(void)
{
   qnx_xusb_poll();
}

input_device_driver_t qnx_joypad = {
   qnx_joypad_init,
   qnx_joypad_query_pad,
   qnx_joypad_destroy,
   qnx_joypad_button,
   qnx_joypad_state,
   qnx_joypad_get_buttons,
   qnx_joypad_axis,
   qnx_joypad_poll,
   qnx_joypad_set_rumble,
   NULL, /* set_rumble_gain */
   NULL, /* set_sensor_state */
   NULL, /* get_sensor_input */
   qnx_joypad_name,
   "qnx",
};
