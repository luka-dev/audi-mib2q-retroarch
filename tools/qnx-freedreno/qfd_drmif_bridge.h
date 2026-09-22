#ifndef RETROARCH_QNX_FREEDRENO_DRMIF_BRIDGE_H
#define RETROARCH_QNX_FREEDRENO_DRMIF_BRIDGE_H

/*
 * Mesa-facing object model for the QNX/GSL transport.
 *
 * This is intentionally independent of Mesa headers.  It establishes and
 * tests the fd_device/fd_pipe/fd_bo/fd_submit/fd_ringbuffer/fd_fence
 * semantics needed by Gallium before the thin Mesa ABI adapter is compiled.
 */

#include "qfd_winsys.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QFD_DRM_MAX_RINGS 64u
#define QFD_DRM_MAX_BOS 256u

typedef struct qfd_drm_device qfd_drm_device_t;
typedef struct qfd_drm_pipe qfd_drm_pipe_t;
typedef struct qfd_drm_bo qfd_drm_bo_t;
typedef struct qfd_drm_submit qfd_drm_submit_t;
typedef struct qfd_drm_ring qfd_drm_ring_t;
typedef struct qfd_drm_fence qfd_drm_fence_t;

enum qfd_drm_param {
   QFD_DRM_PARAM_DEVICE_ID,
   QFD_DRM_PARAM_GMEM_SIZE,
   QFD_DRM_PARAM_GMEM_BASE,
   QFD_DRM_PARAM_GPU_ID,
   QFD_DRM_PARAM_CHIP_ID,
   QFD_DRM_PARAM_TIMESTAMP,
   QFD_DRM_PARAM_NR_PRIORITIES,
   QFD_DRM_PARAM_VA_SIZE
};

enum qfd_drm_bo_access {
   QFD_DRM_BO_READ = 1u << 0,
   QFD_DRM_BO_WRITE = 1u << 1
};

enum qfd_drm_ring_flags {
   QFD_DRM_RING_PRIMARY = 1u << 0
};

typedef struct qfd_drm_profile {
   uint32_t library_flags;
   uint32_t device_flags;
   uint32_t context_type;
   uint32_t context_flags;
   uint32_t allocation_flags;
   uint32_t submit_flags;
   uint32_t clean_operation;
   uint32_t invalidate_operation;
   uint32_t wait_timeout_ms;
} qfd_drm_profile_t;

void qfd_drm_profile_qemu(qfd_drm_profile_t *profile);

int qfd_drm_device_open(const char *library_path,
                        const qfd_drm_profile_t *profile,
                        qfd_drm_device_t **out_device);
int qfd_drm_device_close(qfd_drm_device_t *device);
int qfd_drm_device_get_param(const qfd_drm_device_t *device,
                             enum qfd_drm_param param, uint64_t *out_value);

int qfd_drm_pipe_new(qfd_drm_device_t *device, qfd_drm_pipe_t **out_pipe);
int qfd_drm_pipe_del(qfd_drm_pipe_t *pipe);
int qfd_drm_pipe_get_param(qfd_drm_pipe_t *pipe, enum qfd_drm_param param,
                           uint64_t *out_value);

int qfd_drm_bo_new(qfd_drm_device_t *device, uint32_t size,
                   qfd_drm_bo_t **out_bo);
qfd_drm_bo_t *qfd_drm_bo_ref(qfd_drm_bo_t *bo);
int qfd_drm_bo_del(qfd_drm_bo_t *bo);
void *qfd_drm_bo_map(const qfd_drm_bo_t *bo);
uint32_t qfd_drm_bo_size(const qfd_drm_bo_t *bo);
uint32_t qfd_drm_bo_iova(const qfd_drm_bo_t *bo);
int qfd_drm_bo_cpu_prep(qfd_drm_bo_t *bo, uint32_t access);

int qfd_drm_submit_new(qfd_drm_pipe_t *pipe, qfd_drm_submit_t **out_submit);
int qfd_drm_submit_del(qfd_drm_submit_t *submit);
int qfd_drm_ring_new(qfd_drm_submit_t *submit, uint32_t size,
                     uint32_t flags, qfd_drm_ring_t **out_ring);
int qfd_drm_ring_emit(qfd_drm_ring_t *ring, uint32_t value);
int qfd_drm_ring_emit_many(qfd_drm_ring_t *ring, const uint32_t *values,
                           uint32_t count);
int qfd_drm_ring_reloc(qfd_drm_ring_t *ring, qfd_drm_bo_t *bo,
                       uint32_t offset, int32_t shift, uint32_t or_value,
                       uint32_t access);
uint32_t qfd_drm_ring_dwords(const qfd_drm_ring_t *ring);
int qfd_drm_submit_flush(qfd_drm_submit_t *submit,
                         qfd_drm_fence_t **out_fence);

qfd_drm_fence_t *qfd_drm_fence_ref(qfd_drm_fence_t *fence);
int qfd_drm_fence_wait(qfd_drm_fence_t *fence, uint32_t timeout_ms);
uint32_t qfd_drm_fence_timestamp(const qfd_drm_fence_t *fence);
int qfd_drm_fence_del(qfd_drm_fence_t *fence);

#ifdef __cplusplus
}
#endif

#endif
