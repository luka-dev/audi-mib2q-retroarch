#ifndef GLPASS_RAW_FRAME_H
#define GLPASS_RAW_FRAME_H

#include <stdint.h>

/* The command rings occupy the first rounded 9 MiB of the virt-gpu RAM BAR.
 * A separate mailbox starts here so the Rust software renderer can publish its
 * real RGBA frame without pretending that glClear is image presentation. */
#define GLPASS_RAW_FRAME_PHYS      0x0b900000u
#define GLPASS_RAW_FRAME_OFFSET    0x00900000u
#define GLPASS_RAW_FRAME_MAP_SIZE  0x00200000u
#define GLPASS_RAW_FRAME_MAGIC     0x52464231u /* "RFB1" */
#define GLPASS_RAW_FRAME_VERSION   1u
#define GLPASS_RAW_FRAME_RGBA8888  1u
#define GLPASS_RAW_FRAME_WIDTH     1024u
#define GLPASS_RAW_FRAME_HEIGHT    480u
#define GLPASS_RAW_FRAME_BYTES     (GLPASS_RAW_FRAME_WIDTH * GLPASS_RAW_FRAME_HEIGHT * 4u)

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t data_bytes;
    uint8_t pixels[GLPASS_RAW_FRAME_BYTES];
} glpass_raw_frame;

#endif
