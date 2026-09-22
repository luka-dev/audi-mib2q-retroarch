#ifndef RETROARCH_QNX_GSL_ABI_H
#define RETROARCH_QNX_GSL_ABI_H

/*
 * Recovered from MU1316 libGSLUser.so (SHA-256
 * 3e8d255432e67de7ae517ec5a79921c978c05d1c9d61ffcb0e9a882f8f080b90).
 *
 * This is an ABI description, not a complete or hardware-safe GSL API.
 * All pointer-shaped public fields below describe the original ARM32 ABI.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#define QNX_GSL_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define QNX_GSL_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

typedef uint32_t qnx_gsl_handle32_t;
typedef uint32_t qnx_gsl_ptr32_t;

/* Public ARM32 memory descriptor used by libGSLUser exports. */
typedef struct qnx_gsl_memdesc32 {
   qnx_gsl_ptr32_t hostptr;
   uint32_t reserved04;
   uint64_t gpuaddr;
   uint64_t size;
   uint64_t flags;
} qnx_gsl_memdesc32_t;

/*
 * Public input element accepted by gsl_command_issueib_sync().  The wrapper
 * advances by 0x10 bytes, copies the first 8 bytes to memdesc.gpuaddr, reads
 * sizedwords at +8, and does not consume the final dword on this code path.
 * The hardware protocol ultimately consumes only gpuaddr_lo.
 */
typedef struct qnx_gsl_direct_ib32 {
   uint32_t gpuaddr_lo;
   uint32_t gpuaddr_hi_or_pad;
   uint32_t sizedwords;
   uint32_t ctrl_or_reserved;
} qnx_gsl_direct_ib32_t;

/* Internal-only 32-byte IB produced by gsl_command_issueib_sync(). */
typedef struct qnx_gsl_internal_ib32 {
   qnx_gsl_ptr32_t memdesc;
   uint32_t reserved04;
   uint32_t sizedwords;
   uint32_t reserved0c;
   uint32_t offsetbytes;
   uint32_t reserved14;
   uint32_t reserved18;
   uint32_t reserved1c;
} qnx_gsl_internal_ib32_t;

/* Eight-byte QNX _IO_MSG prefix sent to /dev/kgsl-3D. */
typedef struct qnx_gsl_msg_header {
   uint16_t type;
   uint16_t combine_len;
   uint16_t mgrid;
   uint16_t subtype;
} qnx_gsl_msg_header_t;

/* Packed by field selection, not by memcpy of qnx_gsl_memdesc32_t. */
typedef struct qnx_gsl_wire_memdesc {
   uint32_t hostptr;
   uint32_t gpuaddr;
   uint64_t size;
   uint64_t flags;
} qnx_gsl_wire_memdesc_t;

typedef struct qnx_gsl_submit_fixed_wire {
   uint32_t device;
   uint32_t context;
   uint32_t num_ibs;
   uint32_t flags;
   uint32_t input_timestamp;
} qnx_gsl_submit_fixed_wire_t;

typedef struct qnx_gsl_submit_ib_wire {
   uint32_t gpuaddr;
   uint32_t sizedwords;
} qnx_gsl_submit_ib_wire_t;

enum qnx_gsl_msg_subtype {
   QNX_GSL_MSG_LIBRARY_ENTRY = 0x0900,
   QNX_GSL_MSG_LIBRARY_EXIT = 0x0910,
   QNX_GSL_MSG_DEVICE_OPEN = 0x0920,
   QNX_GSL_MSG_DEVICE_CLOSE = 0x0921,
   QNX_GSL_MSG_DEVICE_GETPROPERTY = 0x0923,
   QNX_GSL_MSG_COMMAND_ISSUEIB = 0x0930,
   QNX_GSL_MSG_COMMAND_READTIMESTAMP = 0x0931,
   QNX_GSL_MSG_COMMAND_WAITTIMESTAMP = 0x0933,
   QNX_GSL_MSG_CONTEXT_CREATE = 0x0950,
   QNX_GSL_MSG_CONTEXT_DESTROY = 0x0951,
   QNX_GSL_MSG_MEMORY_ALLOC = 0x0960,
   QNX_GSL_MSG_MEMORY_FREE = 0x0961,
   QNX_GSL_MSG_MEMORY_CACHEOP = 0x0991
};

enum qnx_gsl_wire_constants {
   QNX_GSL_IO_MSG_TYPE = 0x0113,
   QNX_GSL_IO_MSG_COMBINE_LEN = 8,
   QNX_GSL_IO_MSG_MGRID = 0xf000
};

/* Evidence-backed public signatures needed by an initial standalone probe. */
typedef int32_t (*qnx_gsl_library_open_fn)(uint32_t flags);
typedef int32_t (*qnx_gsl_library_close_fn)(void);
typedef const char *(*qnx_gsl_library_version_fn)(void);
typedef qnx_gsl_handle32_t (*qnx_gsl_device_open_fn)(uint32_t device_id,
                                                      uint32_t flags);
typedef int32_t (*qnx_gsl_device_close_fn)(qnx_gsl_handle32_t device);
typedef int32_t (*qnx_gsl_device_getinfo_fn)(qnx_gsl_handle32_t device,
                                             void *info32);
typedef qnx_gsl_handle32_t (*qnx_gsl_context_create_fn)(
      qnx_gsl_handle32_t device, uint32_t context_type, uint32_t flags);
typedef int32_t (*qnx_gsl_context_destroy_fn)(qnx_gsl_handle32_t device,
                                              qnx_gsl_handle32_t context);
typedef int32_t (*qnx_gsl_memory_alloc_pure_fn)(uint32_t size,
                                                uint32_t flags,
                                                qnx_gsl_memdesc32_t *out);
typedef int32_t (*qnx_gsl_memory_free_pure_fn)(qnx_gsl_memdesc32_t *memdesc);
typedef int32_t (*qnx_gsl_memory_cacheoperation_fn)(
      qnx_gsl_memdesc32_t *memdesc, uint32_t offsetbytes,
      uint32_t sizebytes, uint32_t operation);
typedef int32_t (*qnx_gsl_command_issueib_sync_fn)(
      qnx_gsl_handle32_t device, qnx_gsl_handle32_t context,
      const qnx_gsl_direct_ib32_t *ibs, uint32_t num_ibs,
      uint32_t *timestamp, uint32_t flags, const void *syncobj);
typedef int32_t (*qnx_gsl_command_readtimestamp_fn)(
      qnx_gsl_handle32_t device, qnx_gsl_handle32_t context,
      uint32_t timestamp_type, uint32_t *timestamp);
typedef int32_t (*qnx_gsl_command_waittimestamp_fn)(
      qnx_gsl_handle32_t device, qnx_gsl_handle32_t context,
      uint32_t timestamp, uint32_t timeout);

QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_memdesc32_t) == 32,
                      "MU1316 public memdesc must be 32 bytes");
QNX_GSL_STATIC_ASSERT(offsetof(qnx_gsl_memdesc32_t, gpuaddr) == 8,
                      "MU1316 memdesc gpuaddr offset");
QNX_GSL_STATIC_ASSERT(offsetof(qnx_gsl_memdesc32_t, size) == 16,
                      "MU1316 memdesc size offset");
QNX_GSL_STATIC_ASSERT(offsetof(qnx_gsl_memdesc32_t, flags) == 24,
                      "MU1316 memdesc flags offset");
QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_direct_ib32_t) == 16,
                      "MU1316 direct IB must be 16 bytes");
QNX_GSL_STATIC_ASSERT(offsetof(qnx_gsl_direct_ib32_t, sizedwords) == 8,
                      "MU1316 direct IB sizedwords offset");
QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_internal_ib32_t) == 32,
                      "MU1316 internal IB must be 32 bytes");
QNX_GSL_STATIC_ASSERT(offsetof(qnx_gsl_internal_ib32_t, sizedwords) == 8,
                      "MU1316 internal IB sizedwords offset");
QNX_GSL_STATIC_ASSERT(offsetof(qnx_gsl_internal_ib32_t, offsetbytes) == 16,
                      "MU1316 internal IB offsetbytes offset");
QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_msg_header_t) == 8,
                      "MU1316 QNX message header must be 8 bytes");
QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_wire_memdesc_t) == 24,
                      "MU1316 wire memdesc must be 24 bytes");
QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_submit_fixed_wire_t) == 20,
                      "MU1316 fixed submit payload must be 20 bytes");
QNX_GSL_STATIC_ASSERT(sizeof(qnx_gsl_submit_ib_wire_t) == 8,
                      "MU1316 wire IB must be 8 bytes");

#undef QNX_GSL_STATIC_ASSERT

#endif
