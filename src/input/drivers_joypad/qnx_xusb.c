/*
  QNX XUSB/GIP transport for RetroArch.

  The protocol tables and packet layouts in this file are adapted from the
  SDL HIDAPI Xbox drivers (SDL_hidapi_xbox360.c,
  SDL_hidapi_xbox360w.c, SDL_hidapi_xboxone.c and SDL_hidapi_gip.c).

  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  This is an altered, compact QNX 6.5 port. It exposes stable DInput-style
  physical controls to RetroArch and intentionally omits SDL application
  features which RetroArch's joypad interface cannot consume (audio headset,
  authentication accessories, RGB and battery UI).
*/

#define QNX_XUSB_MAX_DEVICES  QNX_MAX_PADS
#define QNX_XUSB_PACKET_MAX   128
#define QNX_XUSB_QUEUE_DEPTH  16

#define QNX_GIP_CMD_ACKNOWLEDGE 0x01
#define QNX_GIP_CMD_ANNOUNCE    0x02
#define QNX_GIP_CMD_IDENTIFY    0x04
#define QNX_GIP_CMD_VIRTUAL_KEY 0x07
#define QNX_GIP_CMD_RUMBLE      0x09
#define QNX_GIP_CMD_INPUT       0x20
#define QNX_GIP_OPT_ACKNOWLEDGE 0x10
#define QNX_GIP_OPT_INTERNAL    0x20
#define QNX_GIP_OPT_CHUNK       0x80

enum qnx_xusb_protocol
{
   QNX_XUSB_360 = 0,
   QNX_XUSB_360_WIRELESS,
   QNX_XUSB_GIP
};

struct qnx_xusb_packet
{
   uint8_t data[QNX_XUSB_PACKET_MAX];
   uint16_t len;
};

struct qnx_xusb_device
{
   struct usbd_device *device;
   struct usbd_pipe *in_pipe;
   struct usbd_pipe *out_pipe;
   struct usbd_urb *in_urb;
   struct usbd_urb *out_urb;
   uint8_t *in_buffer;
   uint8_t *out_buffer;
   struct qnx_xusb_packet queue[QNX_XUSB_QUEUE_DEPTH];
   usbd_device_instance_t instance;
   enum qnx_xusb_protocol protocol;
   uint64_t attach_time;
   uint16_t vid;
   uint16_t pid;
   uint16_t in_size;
   uint16_t out_size;
   uint8_t queue_head;
   uint8_t queue_tail;
   uint8_t queue_count;
   uint8_t sequence;
   uint8_t slot;
   uint8_t dump_count;
   bool in_bulk;
   bool out_bulk;
   bool output_busy;
   bool controller_present;
   bool gip_started;
   bool reserved;
   bool active;
   char name[128];
};

static struct usbd_connection *qnx_usbd_conn;
static struct qnx_xusb_device qnx_xusb_devices[QNX_XUSB_MAX_DEVICES];
static bool qnx_xusb_shutting_down;

static const uint16_t qnx_xusb_360_vendors[] = {
   0x0079, 0x0351, 0x044f, 0x045e, 0x046d, 0x056e, 0x06a3,
   0x0738, 0x07ff, 0x0e6f, 0x0f0d, 0x1038, 0x11c9, 0x1209,
   0x12ab, 0x1430, 0x146b, 0x1532, 0x15e4, 0x162e, 0x1689,
   0x1949, 0x1bad, 0x20d6, 0x24c6, 0x2c22, 0x2dc8, 0x3537,
   0x3651, 0x37d7, 0x3958, 0x9886
};

static const uint16_t qnx_xusb_gip_vendors[] = {
   0x0351, 0x03f0, 0x044f, 0x045e, 0x0738, 0x0b05, 0x0e6f,
   0x0f0d, 0x10f5, 0x1209, 0x1532, 0x20d6, 0x24c6, 0x294b,
   0x2dc8, 0x2e24, 0x2e95, 0x3285, 0x3537, 0x3651, 0x366c,
   0x3958
};

static bool qnx_xusb_vendor_in(const uint16_t *vendors, size_t count,
      uint16_t vid)
{
   size_t i;
   for (i = 0; i < count; i++)
      if (vendors[i] == vid)
         return true;
   return false;
}

static bool qnx_xusb_is_receiver(uint16_t vid, uint16_t pid)
{
   return vid == 0x045e &&
      (pid == 0x0719 || pid == 0x02a9 || pid == 0x0291);
}

static int16_t qnx_xusb_le16s(const uint8_t *data)
{
   return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint16_t qnx_xusb_le16u(const uint8_t *data)
{
   return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void qnx_xusb_button(input_bits_t *buttons, unsigned index, bool down)
{
   if (down)
      BIT256_SET_PTR(buttons, index);
   else
      BIT256_CLEAR_PTR(buttons, index);
}

static uint8_t qnx_xusb_next_sequence(struct qnx_xusb_device *xdev)
{
   xdev->sequence++;
   if (!xdev->sequence)
      xdev->sequence = 1;
   return xdev->sequence;
}

static int qnx_xusb_setup_transfer(struct usbd_urb *urb, bool bulk,
      uint32_t flags, void *buffer, uint32_t len)
{
   if (bulk)
      return usbd_setup_bulk(urb, flags, buffer, len);
   return usbd_setup_interrupt(urb, flags, buffer, len);
}

static void qnx_xusb_output_done(struct usbd_urb *urb,
      struct usbd_pipe *pipe, void *handle);

static bool qnx_xusb_start_output_locked(struct qnx_xusb_device *xdev)
{
   int result;

   while (xdev->active && xdev->out_pipe && xdev->out_urb &&
          !xdev->output_busy && xdev->queue_count)
   {
      struct qnx_xusb_packet *packet = &xdev->queue[xdev->queue_head];
      memcpy(xdev->out_buffer, packet->data, packet->len);
      result = qnx_xusb_setup_transfer(xdev->out_urb, xdev->out_bulk,
            URB_DIR_OUT, xdev->out_buffer, packet->len);
      if (result == EOK)
         result = usbd_io(xdev->out_urb, xdev->out_pipe,
               qnx_xusb_output_done, xdev, 1000);
      if (result == EOK)
      {
         xdev->output_busy = true;
         return true;
      }

      RARCH_WARN("[QNX XUSB]: output submit failed for %s: %d.\n",
            xdev->name, result);
      xdev->queue_head = (uint8_t)
            ((xdev->queue_head + 1) % QNX_XUSB_QUEUE_DEPTH);
      xdev->queue_count--;
   }
   return xdev->output_busy || xdev->queue_count == 0;
}

static bool qnx_xusb_queue_locked(struct qnx_xusb_device *xdev,
      const uint8_t *data, uint16_t len)
{
   struct qnx_xusb_packet *packet;
   if (!xdev || !xdev->active || !data || !len ||
       len > QNX_XUSB_PACKET_MAX || len > xdev->out_size ||
       xdev->queue_count >= QNX_XUSB_QUEUE_DEPTH)
      return false;

   packet = &xdev->queue[xdev->queue_tail];
   memcpy(packet->data, data, len);
   packet->len = len;
   xdev->queue_tail = (uint8_t)
         ((xdev->queue_tail + 1) % QNX_XUSB_QUEUE_DEPTH);
   xdev->queue_count++;
   qnx_xusb_start_output_locked(xdev);
   return true;
}

static void qnx_xusb_output_done(struct usbd_urb *urb,
      struct usbd_pipe *pipe, void *handle)
{
   struct qnx_xusb_device *xdev = (struct qnx_xusb_device*)handle;
   uint32_t status = 0;
   uint32_t length = 0;
   int result;
   (void)pipe;

   result = usbd_urb_status(urb, &status, &length);
   slock_lock(qnx_hidd_lock);
   if (xdev && xdev->active && xdev->out_urb == urb)
   {
      if (result != EOK ||
          (status & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP)
         RARCH_WARN("[QNX XUSB]: output failed for %s: api %d, "
               "status 0x%08x.\n", xdev->name, result, status);
      if (xdev->queue_count)
      {
         xdev->queue_head = (uint8_t)
               ((xdev->queue_head + 1) % QNX_XUSB_QUEUE_DEPTH);
         xdev->queue_count--;
      }
      xdev->output_busy = false;
      qnx_xusb_start_output_locked(xdev);
   }
   slock_unlock(qnx_hidd_lock);
}

static int qnx_xusb_claim_pad_locked(struct qnx_xusb_device *xdev)
{
   unsigned i;
   for (i = 0; i < QNX_MAX_PADS; i++)
      if (!qnx_pads[i].claimed)
      {
         struct qnx_pad *pad = &qnx_pads[i];
         memset(pad, 0, sizeof(*pad));
         pad->claimed    = true;
         pad->connected  = true;
         pad->generation = ++qnx_generation;
         pad->transport  = QNX_PAD_TRANSPORT_XUSB;
         pad->xusb       = xdev;
         pad->vid        = xdev->vid;
         pad->pid        = xdev->pid;
         pad->num_hats   = 1;
         strlcpy(pad->name, xdev->name, sizeof(pad->name));
         snprintf(pad->phys, sizeof(pad->phys),
               "io-usb/%u.%u/if%u", (unsigned)xdev->instance.path,
               (unsigned)xdev->instance.devno,
               (unsigned)xdev->instance.iface);
         xdev->slot = (uint8_t)i;
         return (int)i;
      }
   return -1;
}

static void qnx_xusb_autoconnect(unsigned slot, const char *name,
      const char *phys, uint16_t vid, uint16_t pid)
{
   RARCH_LOG("[QNX XUSB]: pad %u connected: %s (%04x:%04x).\n",
         slot, name, vid, pid);
   /* The fixed protocol decoder uses this built-in profile name; name remains
    * the real product string for the UI through display_name. */
   input_autoconfigure_connect("QNX XInput Controller", name, phys, "qnx",
         slot, vid, pid);
}

static void qnx_xusb_decode_360_locked(struct qnx_pad *pad,
      const uint8_t *data, uint32_t len)
{
   int32_t left_trigger;
   int32_t right_trigger;
   if (!pad || !data || len < 14 || data[0] != 0x00)
      return;

   BIT256_CLEAR_ALL(pad->buttons);
   qnx_xusb_button(&pad->buttons, 0, data[3] & 0x10); /* A */
   qnx_xusb_button(&pad->buttons, 1, data[3] & 0x20); /* B */
   qnx_xusb_button(&pad->buttons, 2, data[3] & 0x40); /* X */
   qnx_xusb_button(&pad->buttons, 3, data[3] & 0x80); /* Y */
   qnx_xusb_button(&pad->buttons, 4, data[3] & 0x01); /* LB */
   qnx_xusb_button(&pad->buttons, 5, data[3] & 0x02); /* RB */
   qnx_xusb_button(&pad->buttons, 6, data[2] & 0x20); /* Back */
   qnx_xusb_button(&pad->buttons, 7, data[2] & 0x10); /* Start */
   qnx_xusb_button(&pad->buttons, 8, data[2] & 0x40); /* L3 */
   qnx_xusb_button(&pad->buttons, 9, data[2] & 0x80); /* R3 */
   qnx_xusb_button(&pad->buttons, 10, data[3] & 0x04);/* Guide */

   pad->hats[0] = 0;
   if (data[2] & 0x01) pad->hats[0] |= 1 << 0;
   if (data[2] & 0x02) pad->hats[0] |= 1 << 1;
   if (data[2] & 0x04) pad->hats[0] |= 1 << 2;
   if (data[2] & 0x08) pad->hats[0] |= 1 << 3;

   left_trigger  = data[4];
   right_trigger = data[5];
   pad->axes[0] = qnx_xusb_le16s(&data[6]);
   pad->axes[1] = (int16_t)~qnx_xusb_le16s(&data[8]);
   pad->axes[2] = (int16_t)((left_trigger - right_trigger) * 32767 / 255);
   pad->axes[3] = qnx_xusb_le16s(&data[10]);
   pad->axes[4] = (int16_t)~qnx_xusb_le16s(&data[12]);
}

static void qnx_xusb_decode_gip_state_locked(struct qnx_pad *pad,
      const uint8_t *data, uint32_t len)
{
   bool guide;
   int32_t left_trigger;
   int32_t right_trigger;
   if (!pad || !data || len < 14)
      return;

   guide = BIT256_GET(pad->buttons, 10) != 0;
   BIT256_CLEAR_ALL(pad->buttons);
   qnx_xusb_button(&pad->buttons, 0, data[0] & 0x10);
   qnx_xusb_button(&pad->buttons, 1, data[0] & 0x20);
   qnx_xusb_button(&pad->buttons, 2, data[0] & 0x40);
   qnx_xusb_button(&pad->buttons, 3, data[0] & 0x80);
   qnx_xusb_button(&pad->buttons, 4, data[1] & 0x10);
   qnx_xusb_button(&pad->buttons, 5, data[1] & 0x20);
   qnx_xusb_button(&pad->buttons, 6, data[0] & 0x08);
   qnx_xusb_button(&pad->buttons, 7, data[0] & 0x04);
   qnx_xusb_button(&pad->buttons, 8, data[1] & 0x40);
   qnx_xusb_button(&pad->buttons, 9, data[1] & 0x80);
   qnx_xusb_button(&pad->buttons, 10, guide);

   if (len >= 15)
      qnx_xusb_button(&pad->buttons, 11, data[14] & 0x01);
   if (len == 29)
   {
      qnx_xusb_button(&pad->buttons, 12, data[28] & 0x02);
      qnx_xusb_button(&pad->buttons, 13, data[28] & 0x08);
      qnx_xusb_button(&pad->buttons, 14, data[28] & 0x01);
      qnx_xusb_button(&pad->buttons, 15, data[28] & 0x04);
   }
   else if (len == 34)
   {
      qnx_xusb_button(&pad->buttons, 12, data[14] & 0x01);
      qnx_xusb_button(&pad->buttons, 13, data[14] & 0x02);
      qnx_xusb_button(&pad->buttons, 14, data[14] & 0x04);
      qnx_xusb_button(&pad->buttons, 15, data[14] & 0x08);
   }
   else if (len == 46)
   {
      qnx_xusb_button(&pad->buttons, 12, data[18] & 0x01);
      qnx_xusb_button(&pad->buttons, 13, data[18] & 0x02);
      qnx_xusb_button(&pad->buttons, 14, data[18] & 0x04);
      qnx_xusb_button(&pad->buttons, 15, data[18] & 0x08);
      qnx_xusb_button(&pad->buttons, 11, data[28] & 0x01);
   }
   else if (len == 47)
   {
      qnx_xusb_button(&pad->buttons, 12, data[14] & 0x01);
      qnx_xusb_button(&pad->buttons, 13, data[14] & 0x02);
      qnx_xusb_button(&pad->buttons, 14, data[14] & 0x04);
      qnx_xusb_button(&pad->buttons, 15, data[14] & 0x08);
   }

   pad->hats[0] = 0;
   if (data[1] & 0x01) pad->hats[0] |= 1 << 0;
   if (data[1] & 0x02) pad->hats[0] |= 1 << 1;
   if (data[1] & 0x04) pad->hats[0] |= 1 << 2;
   if (data[1] & 0x08) pad->hats[0] |= 1 << 3;

   left_trigger  = qnx_xusb_le16u(&data[2]);
   right_trigger = qnx_xusb_le16u(&data[4]);
   if (left_trigger > 1023) left_trigger = 1023;
   if (right_trigger > 1023) right_trigger = 1023;
   pad->axes[0] = qnx_xusb_le16s(&data[6]);
   pad->axes[1] = (int16_t)~qnx_xusb_le16s(&data[8]);
   pad->axes[2] = (int16_t)((left_trigger - right_trigger) * 32767 / 1023);
   pad->axes[3] = qnx_xusb_le16s(&data[10]);
   pad->axes[4] = (int16_t)~qnx_xusb_le16s(&data[12]);
}

static unsigned qnx_xusb_decode_varint(const uint8_t *data, unsigned len,
      uint32_t *value)
{
   unsigned i;
   uint32_t out = 0;
   for (i = 0; i < 4 && i < len; i++)
   {
      out |= (uint32_t)(data[i] & 0x7f) << (i * 7);
      if (!(data[i] & 0x80))
      {
         *value = out;
         return i + 1;
      }
   }
   return 0;
}

static void qnx_xusb_queue_gip_ack_locked(struct qnx_xusb_device *xdev,
      uint8_t command, uint8_t options, uint8_t sequence,
      uint16_t packet_length)
{
   uint8_t packet[13] = {
      QNX_GIP_CMD_ACKNOWLEDGE, QNX_GIP_OPT_INTERNAL, sequence, 0x09,
      0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
   };
   packet[5]  = command;
   packet[6]  = options & QNX_GIP_OPT_INTERNAL;
   packet[7]  = (uint8_t)packet_length;
   packet[8]  = (uint8_t)(packet_length >> 8);
   qnx_xusb_queue_locked(xdev, packet, sizeof(packet));
}

static void qnx_xusb_start_gip_locked(struct qnx_xusb_device *xdev)
{
   static const uint8_t power_on[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
   static const uint8_t enable_led[] =
      { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
   static const uint8_t security_passed[] =
      { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 };
   static const uint8_t enable_rumble[] =
      { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0f, 0x00, 0x00,
        0x00, 0x00, 0xff, 0x00, 0xeb };
   const uint8_t *packets[] = {
      power_on, enable_led, security_passed, enable_rumble
   };
   const uint8_t lengths[] = {
      sizeof(power_on), sizeof(enable_led),
      sizeof(security_passed), sizeof(enable_rumble)
   };
   uint8_t packet[sizeof(enable_rumble)];
   unsigned i;

   if (xdev->gip_started)
      return;
   xdev->gip_started = true;
   for (i = 0; i < sizeof(packets) / sizeof(packets[0]); i++)
   {
      memcpy(packet, packets[i], lengths[i]);
      packet[2] = qnx_xusb_next_sequence(xdev);
      qnx_xusb_queue_locked(xdev, packet, lengths[i]);
   }
   RARCH_LOG("[QNX XUSB]: queued GIP startup for %s.\n", xdev->name);
}

static void qnx_xusb_decode_gip_locked(struct qnx_xusb_device *xdev,
      struct qnx_pad *pad, const uint8_t *data, uint32_t len)
{
   uint32_t pos = 0;
   while (len - pos >= 4)
   {
      uint8_t command = data[pos++];
      uint8_t options = data[pos++];
      uint8_t sequence = data[pos++];
      uint32_t packet_len = 0;
      uint32_t chunk_offset = 0;
      unsigned consumed = qnx_xusb_decode_varint(&data[pos], len - pos,
            &packet_len);
      const uint8_t *payload;
      if (!consumed)
         return;
      pos += consumed;
      if (options & QNX_GIP_OPT_CHUNK)
      {
         consumed = qnx_xusb_decode_varint(&data[pos], len - pos,
               &chunk_offset);
         if (!consumed)
            return;
         pos += consumed;
      }
      if (packet_len > len - pos)
         return;
      payload = &data[pos];

      if (options & QNX_GIP_OPT_ACKNOWLEDGE)
         qnx_xusb_queue_gip_ack_locked(xdev, command, options, sequence,
               (uint16_t)(chunk_offset + packet_len));

      if (!(options & 0x0f))
      {
         if (options & QNX_GIP_OPT_INTERNAL)
         {
            if (command == QNX_GIP_CMD_ANNOUNCE)
            {
               uint8_t identify[] = { QNX_GIP_CMD_IDENTIFY,
                  QNX_GIP_OPT_INTERNAL, 0x00, 0x00 };
               identify[2] = qnx_xusb_next_sequence(xdev);
               qnx_xusb_queue_locked(xdev, identify, sizeof(identify));
            }
            else if (command == QNX_GIP_CMD_IDENTIFY)
               qnx_xusb_start_gip_locked(xdev);
            else if (command == QNX_GIP_CMD_VIRTUAL_KEY && pad && packet_len)
               qnx_xusb_button(&pad->buttons, 10, payload[0] & 0x01);
         }
         else if (command == QNX_GIP_CMD_INPUT && pad)
         {
            qnx_xusb_decode_gip_state_locked(pad, payload, packet_len);
            if (!xdev->gip_started)
               qnx_xusb_start_gip_locked(xdev);
         }
      }
      pos += packet_len;
   }
}

static int qnx_xusb_submit_input_locked(struct qnx_xusb_device *xdev);

static void qnx_xusb_input_done(struct usbd_urb *urb,
      struct usbd_pipe *pipe, void *handle)
{
   struct qnx_xusb_device *xdev = (struct qnx_xusb_device*)handle;
   uint32_t status = 0;
   uint32_t length = 0;
   unsigned connect_slot = QNX_INVALID_SLOT;
   unsigned disconnect_slot = QNX_INVALID_SLOT;
   char connect_name[128] = {0};
   char connect_phys[64] = {0};
   uint16_t connect_vid = 0;
   uint16_t connect_pid = 0;
   int result;
   (void)pipe;

   result = usbd_urb_status(urb, &status, &length);
   slock_lock(qnx_hidd_lock);
   if (!xdev || !xdev->active || xdev->in_urb != urb)
   {
      slock_unlock(qnx_hidd_lock);
      return;
   }

   if (result == EOK &&
       (status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP && length)
   {
      struct qnx_pad *pad = xdev->slot < QNX_MAX_PADS
         ? &qnx_pads[xdev->slot] : NULL;

      if (qnx_dump_reports && xdev->dump_count < 8)
      {
         char line[3 * 32 + 1];
         uint32_t i;
         uint32_t n = length < 32 ? length : 32;
         size_t at = 0;
         for (i = 0; i < n && at + 3 < sizeof(line); i++)
            at += (size_t)snprintf(line + at, sizeof(line) - at,
                  "%02x%s", xdev->in_buffer[i], i + 1 == n ? "" : " ");
         RARCH_LOG("[QNX XUSB]: %s raw[%u]: %s\n",
               xdev->name, (unsigned)length, line);
         xdev->dump_count++;
      }

      if (xdev->protocol == QNX_XUSB_360_WIRELESS &&
          length == 2 && xdev->in_buffer[0] == 0x08)
      {
         bool present = (xdev->in_buffer[1] & 0x80) != 0;
         if (present && !xdev->controller_present)
         {
            int slot;
            xdev->controller_present = true;
            slot = qnx_xusb_claim_pad_locked(xdev);
            if (slot >= 0)
            {
               connect_slot = (unsigned)slot;
               strlcpy(connect_name, qnx_pads[slot].name,
                     sizeof(connect_name));
               strlcpy(connect_phys, qnx_pads[slot].phys,
                     sizeof(connect_phys));
               connect_vid = xdev->vid;
               connect_pid = xdev->pid;
            }
         }
         else if (!present && xdev->controller_present)
         {
            xdev->controller_present = false;
            if (xdev->slot < QNX_MAX_PADS)
            {
               disconnect_slot = xdev->slot;
               memset(&qnx_pads[xdev->slot], 0,
                     sizeof(qnx_pads[xdev->slot]));
               xdev->slot = QNX_INVALID_SLOT;
            }
         }
      }
      else if (pad && pad->connected)
      {
         if (xdev->protocol == QNX_XUSB_360)
            qnx_xusb_decode_360_locked(pad, xdev->in_buffer, length);
         else if (xdev->protocol == QNX_XUSB_360_WIRELESS &&
                  length >= 18 && xdev->in_buffer[0] == 0x00 &&
                  (xdev->in_buffer[1] & 0x01))
            qnx_xusb_decode_360_locked(pad, xdev->in_buffer + 4,
                  length - 4);
         else if (xdev->protocol == QNX_XUSB_GIP)
            qnx_xusb_decode_gip_locked(xdev, pad, xdev->in_buffer, length);
      }
   }
   else if (qnx_debug)
      RARCH_WARN("[QNX XUSB]: input completion failed for %s: api %d, "
            "status 0x%08x.\n", xdev->name, result, status);

   if (xdev->active)
   {
      if ((status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP_ERR)
         usbd_reset_pipe(xdev->in_pipe);
      result = qnx_xusb_submit_input_locked(xdev);
      if (result != EOK)
         RARCH_WARN("[QNX XUSB]: input resubmit failed for %s: %d.\n",
               xdev->name, result);
   }
   slock_unlock(qnx_hidd_lock);

   if (connect_slot < QNX_MAX_PADS)
      qnx_xusb_autoconnect(connect_slot, connect_name, connect_phys,
            connect_vid, connect_pid);
   if (disconnect_slot < QNX_MAX_PADS)
   {
      RARCH_LOG("[QNX XUSB]: wireless pad %u disconnected.\n",
            disconnect_slot);
      input_autoconfigure_disconnect(disconnect_slot, "qnx");
   }
}

static int qnx_xusb_submit_input_locked(struct qnx_xusb_device *xdev)
{
   int result = qnx_xusb_setup_transfer(xdev->in_urb, xdev->in_bulk,
         URB_DIR_IN | URB_SHORT_XFER_OK, xdev->in_buffer, xdev->in_size);
   if (result == EOK)
      result = usbd_io(xdev->in_urb, xdev->in_pipe, qnx_xusb_input_done,
            xdev, USBD_TIME_INFINITY);
   return result;
}

static bool qnx_xusb_match(usbd_interface_descriptor_t *iface,
      uint16_t vid, uint16_t pid, enum qnx_xusb_protocol *protocol)
{
   if (!iface || iface->bInterfaceClass != 0xff)
      return false;

   if (iface->bInterfaceSubClass == 0x5d &&
       iface->bInterfaceProtocol == 0x01 &&
       qnx_xusb_vendor_in(qnx_xusb_360_vendors,
          sizeof(qnx_xusb_360_vendors) / sizeof(qnx_xusb_360_vendors[0]),
          vid))
   {
      *protocol = QNX_XUSB_360;
      return true;
   }
   if (iface->bInterfaceSubClass == 0x5d &&
       (iface->bInterfaceProtocol == 0x81 ||
        (iface->bInterfaceProtocol == 0 && qnx_xusb_is_receiver(vid, pid))))
   {
      *protocol = QNX_XUSB_360_WIRELESS;
      return true;
   }
   if (iface->bInterfaceNumber == 0 &&
       iface->bInterfaceSubClass == 0x47 &&
       iface->bInterfaceProtocol == 0xd0 &&
       qnx_xusb_vendor_in(qnx_xusb_gip_vendors,
          sizeof(qnx_xusb_gip_vendors) / sizeof(qnx_xusb_gip_vendors[0]),
          vid))
   {
      *protocol = QNX_XUSB_GIP;
      return true;
   }
   return false;
}

static void qnx_xusb_cleanup(struct qnx_xusb_device *xdev)
{
   unsigned disconnected = QNX_INVALID_SLOT;
   if (!xdev || (!xdev->active && !xdev->reserved))
      return;

   slock_lock(qnx_hidd_lock);
   xdev->active = false;
   if (xdev->slot < QNX_MAX_PADS)
   {
      disconnected = xdev->slot;
      memset(&qnx_pads[xdev->slot], 0, sizeof(qnx_pads[xdev->slot]));
      xdev->slot = QNX_INVALID_SLOT;
   }
   slock_unlock(qnx_hidd_lock);

   if (xdev->in_pipe) usbd_abort_pipe(xdev->in_pipe);
   if (xdev->out_pipe) usbd_abort_pipe(xdev->out_pipe);
   if (xdev->in_pipe) usbd_close_pipe(xdev->in_pipe);
   if (xdev->out_pipe) usbd_close_pipe(xdev->out_pipe);
   if (xdev->in_urb) usbd_free_urb(xdev->in_urb);
   if (xdev->out_urb) usbd_free_urb(xdev->out_urb);
   if (xdev->in_buffer) usbd_free(xdev->in_buffer);
   if (xdev->out_buffer) usbd_free(xdev->out_buffer);
   if (xdev->device) usbd_detach(xdev->device);

   if (disconnected < QNX_MAX_PADS)
   {
      RARCH_LOG("[QNX XUSB]: pad %u disconnected (%s).\n",
            disconnected, xdev->name);
      input_autoconfigure_disconnect(disconnected, "qnx");
   }
   memset(xdev, 0, sizeof(*xdev));
   xdev->slot = QNX_INVALID_SLOT;
}

static void qnx_xusb_insertion(struct usbd_connection *connection,
      usbd_device_instance_t *instance)
{
   struct qnx_xusb_device *xdev = NULL;
   struct usbd_desc_node *node = NULL;
   struct usbd_desc_node *device_node = NULL;
   usbd_interface_descriptor_t *iface;
   usbd_device_descriptor_t *device_desc;
   enum qnx_xusb_protocol protocol;
   unsigned i;
   int result;
   int pad_slot = -1;
   char pad_name[128] = {0};
   char pad_phys[64] = {0};

   if (!instance || qnx_xusb_shutting_down)
      return;

   /* Fast rejection avoids attaching to ordinary HID/storage devices. */
   if (instance->ident.dclass != 0x00 &&
       instance->ident.dclass != 0xff &&
       !qnx_xusb_is_receiver((uint16_t)instance->ident.vendor,
          (uint16_t)instance->ident.device))
      return;

   slock_lock(qnx_hidd_lock);
   for (i = 0; i < QNX_XUSB_MAX_DEVICES; i++)
      if (!qnx_xusb_devices[i].active && !qnx_xusb_devices[i].reserved)
      {
         xdev = &qnx_xusb_devices[i];
         memset(xdev, 0, sizeof(*xdev));
         xdev->reserved = true;
         xdev->slot = QNX_INVALID_SLOT;
         xdev->instance = *instance;
         break;
      }
   slock_unlock(qnx_hidd_lock);
   if (!xdev)
      return;

   result = usbd_attach(connection, instance, 0, &xdev->device);
   if (result != EOK || !xdev->device)
      goto failed;

   iface = usbd_interface_descriptor(xdev->device,
         (uint8_t)instance->config, (uint8_t)instance->iface,
         (uint8_t)instance->alternate, &node);
   if (!iface || !qnx_xusb_match(iface,
         (uint16_t)instance->ident.vendor,
         (uint16_t)instance->ident.device, &protocol))
      goto failed;

   if (instance->config &&
       usbd_select_config(xdev->device, (uint8_t)instance->config) != EOK)
      goto failed;
   if (usbd_select_interface(xdev->device, (uint8_t)instance->iface,
         (uint8_t)instance->alternate) != EOK)
      goto failed;

   xdev->vid      = (uint16_t)instance->ident.vendor;
   xdev->pid      = (uint16_t)instance->ident.device;
   xdev->protocol = protocol;
   xdev->in_size  = QNX_XUSB_PACKET_MAX;
   xdev->out_size = QNX_XUSB_PACKET_MAX;

   for (i = 0; i < iface->bNumEndpoints; i++)
   {
      struct usbd_desc_node *endpoint_node = NULL;
      usbd_endpoint_descriptor_t *endpoint = usbd_endpoint_descriptor(
            xdev->device, (uint8_t)instance->config,
            (uint8_t)instance->iface, (uint8_t)instance->alternate,
            (uint8_t)i, &endpoint_node);
      uint8_t type;
      if (!endpoint)
         continue;
      type = endpoint->bmAttributes & 0x03;
      if (type != USB_ATTRIB_INTERRUPT && type != USB_ATTRIB_BULK)
         continue;
      if ((endpoint->bEndpointAddress & USB_ENDPOINT_IN) && !xdev->in_pipe)
      {
         if (usbd_open_pipe(xdev->device, (usbd_descriptors_t*)endpoint,
               &xdev->in_pipe) == EOK)
         {
            xdev->in_bulk = type == USB_ATTRIB_BULK;
            if (endpoint->wMaxPacketSize &&
                endpoint->wMaxPacketSize < xdev->in_size)
               xdev->in_size = endpoint->wMaxPacketSize;
         }
      }
      else if (!(endpoint->bEndpointAddress & USB_ENDPOINT_IN) &&
               !xdev->out_pipe)
      {
         if (usbd_open_pipe(xdev->device, (usbd_descriptors_t*)endpoint,
               &xdev->out_pipe) == EOK)
         {
            xdev->out_bulk = type == USB_ATTRIB_BULK;
            if (endpoint->wMaxPacketSize &&
                endpoint->wMaxPacketSize < xdev->out_size)
               xdev->out_size = endpoint->wMaxPacketSize;
         }
      }
   }
   if (!xdev->in_pipe || !xdev->out_pipe)
      goto failed;

   xdev->in_urb     = usbd_alloc_urb(NULL);
   xdev->out_urb    = usbd_alloc_urb(NULL);
   xdev->in_buffer  = (uint8_t*)usbd_alloc(QNX_XUSB_PACKET_MAX);
   xdev->out_buffer = (uint8_t*)usbd_alloc(QNX_XUSB_PACKET_MAX);
   if (!xdev->in_urb || !xdev->out_urb ||
       !xdev->in_buffer || !xdev->out_buffer)
      goto failed;

   device_desc = usbd_device_descriptor(xdev->device, &device_node);
   if (device_desc && device_desc->iProduct)
   {
      const char *product = usbd_string(xdev->device,
            device_desc->iProduct, 0);
      if (product && *product)
         strlcpy(xdev->name, product, sizeof(xdev->name));
   }
   if (!xdev->name[0])
   {
      if (protocol == QNX_XUSB_360_WIRELESS)
         strlcpy(xdev->name, "Xbox 360 Wireless Controller",
               sizeof(xdev->name));
      else if (protocol == QNX_XUSB_GIP)
         strlcpy(xdev->name, "Xbox One/Series Controller",
               sizeof(xdev->name));
      else
         strlcpy(xdev->name, "Xbox 360 Compatible Controller",
               sizeof(xdev->name));
   }

   slock_lock(qnx_hidd_lock);
   xdev->active      = true;
   xdev->reserved    = false;
   xdev->attach_time = cpu_features_get_time_usec();
   if (protocol != QNX_XUSB_360_WIRELESS)
   {
      pad_slot = qnx_xusb_claim_pad_locked(xdev);
      if (pad_slot >= 0)
      {
         strlcpy(pad_name, qnx_pads[pad_slot].name, sizeof(pad_name));
         strlcpy(pad_phys, qnx_pads[pad_slot].phys, sizeof(pad_phys));
      }
   }
   if (protocol == QNX_XUSB_360_WIRELESS)
   {
      const uint8_t presence[] =
         { 0x08, 0x00, 0x0f, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0 };
      qnx_xusb_queue_locked(xdev, presence, sizeof(presence));
   }
   else if (protocol == QNX_XUSB_360)
   {
      uint8_t led[] = { 0x01, 0x03, 0x06 };
      qnx_xusb_queue_locked(xdev, led, sizeof(led));
   }
   else if (xdev->vid == 0x0e6f || xdev->vid == 0x20d6 ||
            xdev->vid == 0x24c6)
      qnx_xusb_start_gip_locked(xdev);

   result = qnx_xusb_submit_input_locked(xdev);
   slock_unlock(qnx_hidd_lock);
   if (result != EOK)
      goto failed;

   RARCH_LOG("[QNX XUSB]: claimed io-usb interface %u for %s "
         "(%04x:%04x, protocol %s).\n", (unsigned)instance->iface,
         xdev->name, xdev->vid, xdev->pid,
         protocol == QNX_XUSB_360 ? "XUSB" :
         protocol == QNX_XUSB_360_WIRELESS ? "XUSB wireless" : "GIP");
   if (pad_slot >= 0)
      qnx_xusb_autoconnect((unsigned)pad_slot, pad_name, pad_phys,
            xdev->vid, xdev->pid);
   return;

failed:
   if (qnx_debug)
      RARCH_WARN("[QNX XUSB]: rejected/failed USB interface %u "
            "(%04x:%04x), error %d.\n", (unsigned)instance->iface,
            (unsigned)instance->ident.vendor,
            (unsigned)instance->ident.device, result);
   qnx_xusb_cleanup(xdev);
}

static void qnx_xusb_removal(struct usbd_connection *connection,
      usbd_device_instance_t *instance)
{
   unsigned i;
   (void)connection;
   if (!instance)
      return;
   for (i = 0; i < QNX_XUSB_MAX_DEVICES; i++)
   {
      struct qnx_xusb_device *xdev = &qnx_xusb_devices[i];
      if ((xdev->active || xdev->reserved) &&
          xdev->instance.path == instance->path &&
          xdev->instance.devno == instance->devno &&
          xdev->instance.generation == instance->generation &&
          xdev->instance.iface == instance->iface)
      {
         qnx_xusb_cleanup(xdev);
         return;
      }
   }
}

static void qnx_xusb_event(struct usbd_connection *connection,
      usbd_device_instance_t *instance, uint16_t type)
{
   (void)connection;
   (void)instance;
   if (qnx_debug)
      RARCH_DBG("[QNX XUSB]: io-usb event %u.\n", (unsigned)type);
}

static bool qnx_xusb_init(void)
{
   static usbd_funcs_t functions;
   static usbd_device_ident_t interest;
   usbd_connect_parm_t parameters;
   unsigned i;

   qnx_xusb_shutting_down = false;
   memset(qnx_xusb_devices, 0, sizeof(qnx_xusb_devices));
   for (i = 0; i < QNX_XUSB_MAX_DEVICES; i++)
      qnx_xusb_devices[i].slot = QNX_INVALID_SLOT;

   memset(&functions, 0, sizeof(functions));
   functions.nentries  = _USBDI_NFUNCS;
   functions.insertion = qnx_xusb_insertion;
   functions.removal   = qnx_xusb_removal;
   functions.event     = qnx_xusb_event;

   interest.vendor   = USBD_CONNECT_WILDCARD;
   interest.device   = USBD_CONNECT_WILDCARD;
   interest.dclass   = USBD_CONNECT_WILDCARD;
   interest.subclass = USBD_CONNECT_WILDCARD;
   interest.protocol = USBD_CONNECT_WILDCARD;

   memset(&parameters, 0, sizeof(parameters));
   parameters.path         = NULL;
   parameters.vusb         = USB_VERSION;
   parameters.vusbd        = USBD_VERSION;
   parameters.evtbufsz     = 4096;
   parameters.ident        = &interest;
   parameters.funcs        = &functions;
   parameters.connect_wait = 1;

   if (usbd_connect(&parameters, &qnx_usbd_conn) != EOK)
   {
      qnx_usbd_conn = NULL;
      RARCH_WARN("[QNX XUSB]: could not connect to /dev/io-usb/io-usb.\n");
      return false;
   }
   RARCH_LOG("[QNX XUSB]: connected to io-usb for XUSB/GIP controllers.\n");
   return true;
}

static void qnx_xusb_destroy(void)
{
   unsigned i;
   qnx_xusb_shutting_down = true;
   for (i = 0; i < QNX_XUSB_MAX_DEVICES; i++)
      qnx_xusb_cleanup(&qnx_xusb_devices[i]);
   if (qnx_usbd_conn)
      usbd_disconnect(qnx_usbd_conn);
   qnx_usbd_conn = NULL;
}

/* Called with qnx_hidd_lock held by qnx_joypad_set_rumble(). */
static bool qnx_xusb_set_rumble(struct qnx_pad *pad,
      enum retro_rumble_effect effect, uint16_t strength)
{
   struct qnx_xusb_device *xdev;
   uint16_t strong;
   uint16_t weak;
   uint8_t packet[13];
   uint16_t packet_len;

   if (!pad || pad->transport != QNX_PAD_TRANSPORT_XUSB ||
       !(xdev = pad->xusb) || !xdev->active)
      return false;
   strong = pad->rumble_strong;
   weak   = pad->rumble_weak;
   if (effect == RETRO_RUMBLE_STRONG)
      strong = strength;
   else if (effect == RETRO_RUMBLE_WEAK)
      weak = strength;
   else
      return false;

   if (strong == pad->rumble_strong && weak == pad->rumble_weak)
      return true;

   memset(packet, 0, sizeof(packet));
   if (xdev->protocol == QNX_XUSB_360)
   {
      const uint8_t base[] = { 0x00, 0x08, 0x00, 0, 0, 0, 0, 0 };
      memcpy(packet, base, sizeof(base));
      packet[3] = (uint8_t)(strong >> 8);
      packet[4] = (uint8_t)(weak >> 8);
      packet_len = sizeof(base);
   }
   else if (xdev->protocol == QNX_XUSB_360_WIRELESS)
   {
      const uint8_t base[] =
         { 0x00, 0x01, 0x0f, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0 };
      memcpy(packet, base, sizeof(base));
      packet[5] = (uint8_t)(strong >> 8);
      packet[6] = (uint8_t)(weak >> 8);
      packet_len = sizeof(base);
   }
   else
   {
      const uint8_t base[] =
         { QNX_GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
           0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xeb };
      memcpy(packet, base, sizeof(base));
      packet[2] = qnx_xusb_next_sequence(xdev);
      packet[8] = (uint8_t)(strong / 655); /* GIP magnitude is 0..100 */
      packet[9] = (uint8_t)(weak / 655);
      packet_len = sizeof(base);
   }

   if (!qnx_xusb_queue_locked(xdev, packet, packet_len))
      return false;
   pad->rumble_strong = strong;
   pad->rumble_weak   = weak;
   return true;
}

static void qnx_xusb_poll(void)
{
   unsigned i;
   uint64_t now = cpu_features_get_time_usec();
   if (!qnx_hidd_lock)
      return;
   slock_lock(qnx_hidd_lock);
   for (i = 0; i < QNX_XUSB_MAX_DEVICES; i++)
   {
      struct qnx_xusb_device *xdev = &qnx_xusb_devices[i];
      if (xdev->active && xdev->protocol == QNX_XUSB_GIP &&
          !xdev->gip_started && now - xdev->attach_time >= 250000)
         qnx_xusb_start_gip_locked(xdev);
   }
   slock_unlock(qnx_hidd_lock);
}
