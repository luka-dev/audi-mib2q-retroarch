#ifndef RETROARCH_QNX_FREEDRENO_WINSYS_H
#define RETROARCH_QNX_FREEDRENO_WINSYS_H

/*
 * Small QNX/GSL transport layer for a future Mesa Freedreno A3xx port.
 *
 * This interface deliberately exposes raw GSL flags until their meanings are
 * verified on MU1316.  It owns no Screen/EGL objects and does not generate PM4.
 */

#include "qnx_gsl_abi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QFD_DEVICE_INFO_WORDS 8u
#define QFD_MAX_SUBMIT_IBS 64u

typedef struct qfd_device qfd_device_t;
typedef struct qfd_pipe qfd_pipe_t;
typedef struct qfd_bo qfd_bo_t;

typedef struct qfd_device_info {
   uint32_t words[QFD_DEVICE_INFO_WORDS];
} qfd_device_info_t;

/*
 * Injectable dispatch boundary.  Production uses qfd_device_open(), while
 * host tests and a possible direct MsgSendv transport use this entry point.
 */
typedef struct qfd_gsl_dispatch {
   qnx_gsl_library_open_fn library_open;
   qnx_gsl_library_close_fn library_close;
   qnx_gsl_library_version_fn library_version;
   qnx_gsl_device_open_fn device_open;
   qnx_gsl_device_close_fn device_close;
   qnx_gsl_device_getinfo_fn device_getinfo;
   qnx_gsl_context_create_fn context_create;
   qnx_gsl_context_destroy_fn context_destroy;
   qnx_gsl_memory_alloc_pure_fn memory_alloc_pure;
   qnx_gsl_memory_free_pure_fn memory_free_pure;
   qnx_gsl_memory_cacheoperation_fn memory_cacheoperation;
   qnx_gsl_command_issueib_sync_fn command_issueib_sync;
   qnx_gsl_command_readtimestamp_fn command_readtimestamp;
   qnx_gsl_command_waittimestamp_fn command_waittimestamp;
} qfd_gsl_dispatch_t;

typedef struct qfd_ib {
   const qfd_bo_t *bo;
   uint32_t offset_bytes;
   uint32_t size_dwords;
} qfd_ib_t;

/* Stock GLES creates a 3D context with type 2.  Flags remain explicit. */
enum qfd_context_type {
   QFD_CONTEXT_TYPE_3D = 2
};

/* Observed operation numbers; semantic names still require a live round-trip. */
enum qfd_cache_operation {
   QFD_CACHE_OPERATION_1 = 1,
   QFD_CACHE_OPERATION_2 = 2
};

int qfd_device_open(const char *library_path, uint32_t library_flags,
                    uint32_t device_flags, qfd_device_t **out_device);
int qfd_device_open_dispatch(const qfd_gsl_dispatch_t *dispatch,
                             uint32_t library_flags,
                             uint32_t device_flags,
                             qfd_device_t **out_device);
int qfd_device_close(qfd_device_t *device);

const char *qfd_device_library_version(const qfd_device_t *device);
qnx_gsl_handle32_t qfd_device_handle(const qfd_device_t *device);
void qfd_device_get_info(const qfd_device_t *device,
                         qfd_device_info_t *out_info);

int qfd_pipe_new(qfd_device_t *device, uint32_t context_type,
                 uint32_t context_flags, qfd_pipe_t **out_pipe);
int qfd_pipe_del(qfd_pipe_t *pipe);
qnx_gsl_handle32_t qfd_pipe_handle(const qfd_pipe_t *pipe);

int qfd_bo_new(qfd_device_t *device, uint32_t size, uint32_t allocation_flags,
               qfd_bo_t **out_bo);
int qfd_bo_del(qfd_bo_t *bo);
void *qfd_bo_map(const qfd_bo_t *bo);
uint32_t qfd_bo_gpuaddr(const qfd_bo_t *bo);
uint32_t qfd_bo_size(const qfd_bo_t *bo);
uint64_t qfd_bo_flags(const qfd_bo_t *bo);

int qfd_bo_cache(qfd_bo_t *bo, uint32_t offset_bytes, uint32_t size_bytes,
                 uint32_t operation);

int qfd_pipe_submit(qfd_pipe_t *pipe, const qfd_ib_t *ibs, uint32_t num_ibs,
                    uint32_t submit_flags, const void *syncobj,
                    uint32_t *inout_timestamp);
int qfd_pipe_read_timestamp(qfd_pipe_t *pipe, uint32_t timestamp_type,
                            uint32_t *out_timestamp);
int qfd_pipe_wait_timestamp(qfd_pipe_t *pipe, uint32_t timestamp,
                            uint32_t timeout);

/* Timestamp comparison that remains valid across uint32_t rollover. */
int qfd_timestamp_after(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif
