/*
 * Frame codec shared by guest and host. One frame on the byte stream:
 *
 *     [u16 op][u32 len][payload len bytes]
 *
 * Round-trip replies use the same writer with op=0 (reply marker):
 *     [u16 0][u32 rlen][reply rlen bytes]
 *
 * Pure read()/write() on a fd, so the SAME code drives:
 *   - a Unix socket (host selftest / chardev bridge on macOS)
 *   - /dev/virtio-ports/glpass (QNX guest, milestone 1)
 *
 * Blocking, length-framed, no partial-frame ambiguity: read_frame loops until
 * the whole frame is in. Little-endian on the wire (armle guest + LE hosts).
 */
#ifndef GLPASS_WIRE_H
#define GLPASS_WIRE_H

#include <stdint.h>
#include <stddef.h>

#define WIRE_OP_REPLY 0

/* returns 0 on success, -1 on short write / error */
int wire_write_frame(int fd, uint16_t op, const void *payload, uint32_t len);

/* reads one frame: sets *op and copies up to cap payload bytes into buf,
 * sets *len to the real payload length. returns 0 ok, -1 eof/error.
 * if payload > cap it still consumes the whole frame but truncates into buf. */
int wire_read_frame(int fd, uint16_t *op, void *buf, uint32_t cap, uint32_t *len);

#endif /* GLPASS_WIRE_H */
