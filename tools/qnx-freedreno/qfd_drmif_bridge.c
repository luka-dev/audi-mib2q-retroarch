#include "qfd_drmif_bridge.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct qfd_drm_device {
   qfd_device_t *transport;
   qfd_device_info_t info;
   qfd_drm_profile_t profile;
   uint32_t pipe_count;
   uint32_t bo_count;
};

struct qfd_drm_pipe {
   qfd_drm_device_t *device;
   qfd_pipe_t *transport;
   uint32_t submit_count;
   uint32_t fence_count;
};

struct qfd_drm_bo {
   qfd_drm_device_t *device;
   qfd_bo_t *transport;
   uint32_t refcount;
};

typedef struct qfd_drm_bo_use {
   qfd_drm_bo_t *bo;
   uint32_t access;
} qfd_drm_bo_use_t;

struct qfd_drm_ring {
   qfd_drm_submit_t *submit;
   qfd_drm_bo_t *bo;
   uint32_t *start;
   uint32_t capacity_dwords;
   uint32_t used_dwords;
   uint32_t flags;
};

struct qfd_drm_submit {
   qfd_drm_pipe_t *pipe;
   qfd_drm_ring_t *rings[QFD_DRM_MAX_RINGS];
   qfd_drm_bo_use_t bos[QFD_DRM_MAX_BOS];
   uint32_t ring_count;
   uint32_t bo_count;
   qfd_drm_ring_t *primary;
   int flushed;
};

struct qfd_drm_fence {
   qfd_drm_pipe_t *pipe;
   qfd_drm_bo_use_t bos[QFD_DRM_MAX_BOS];
   uint32_t bo_count;
   uint32_t timestamp;
   uint32_t refcount;
   int waited;
};

void qfd_drm_profile_qemu(qfd_drm_profile_t *profile)
{
   if (profile == NULL)
      return;
   memset(profile, 0, sizeof(*profile));
   profile->context_type = QFD_CONTEXT_TYPE_3D;
   profile->clean_operation = QFD_CACHE_OPERATION_1;
   profile->invalidate_operation = QFD_CACHE_OPERATION_2;
   profile->wait_timeout_ms = 5000u;
}

int qfd_drm_device_open(const char *library_path,
                        const qfd_drm_profile_t *profile,
                        qfd_drm_device_t **out_device)
{
   qfd_drm_device_t *device;
   int status;

   if (profile == NULL || out_device == NULL)
      return -EINVAL;
   *out_device = NULL;
   if (profile->context_type == 0 || profile->clean_operation == 0 ||
       profile->invalidate_operation == 0)
      return -EINVAL;

   device = (qfd_drm_device_t *)calloc(1, sizeof(*device));
   if (device == NULL)
      return -ENOMEM;
   device->profile = *profile;
   status = qfd_device_open(library_path, profile->library_flags,
                            profile->device_flags, &device->transport);
   if (status != 0)
   {
      free(device);
      return status;
   }
   qfd_device_get_info(device->transport, &device->info);
   *out_device = device;
   return 0;
}

int qfd_drm_device_close(qfd_drm_device_t *device)
{
   int status;

   if (device == NULL)
      return 0;
   if (device->pipe_count != 0 || device->bo_count != 0)
      return -EBUSY;
   status = qfd_device_close(device->transport);
   if (status != 0)
      return status;
   memset(device, 0, sizeof(*device));
   free(device);
   return 0;
}

int qfd_drm_device_get_param(const qfd_drm_device_t *device,
                             enum qfd_drm_param param, uint64_t *out_value)
{
   uint64_t value;

   if (device == NULL || out_value == NULL)
      return -EINVAL;
   switch (param)
   {
      case QFD_DRM_PARAM_DEVICE_ID:
         value = 1;
         break;
      case QFD_DRM_PARAM_GMEM_SIZE:
         value = device->info.words[7];
         break;
      case QFD_DRM_PARAM_GMEM_BASE:
         value = device->info.words[4];
         break;
      case QFD_DRM_PARAM_GPU_ID:
         value = device->info.words[6];
         break;
      case QFD_DRM_PARAM_CHIP_ID:
         value = device->info.words[1];
         break;
      case QFD_DRM_PARAM_NR_PRIORITIES:
         value = 1;
         break;
      case QFD_DRM_PARAM_VA_SIZE:
         value = 32;
         break;
      default:
         return -EINVAL;
   }
   *out_value = value;
   return 0;
}

int qfd_drm_pipe_new(qfd_drm_device_t *device, qfd_drm_pipe_t **out_pipe)
{
   qfd_drm_pipe_t *pipe;
   int status;

   if (device == NULL || out_pipe == NULL)
      return -EINVAL;
   *out_pipe = NULL;
   pipe = (qfd_drm_pipe_t *)calloc(1, sizeof(*pipe));
   if (pipe == NULL)
      return -ENOMEM;
   pipe->device = device;
   status = qfd_pipe_new(device->transport, device->profile.context_type,
                         device->profile.context_flags, &pipe->transport);
   if (status != 0)
   {
      free(pipe);
      return status;
   }
   device->pipe_count++;
   *out_pipe = pipe;
   return 0;
}

int qfd_drm_pipe_del(qfd_drm_pipe_t *pipe)
{
   qfd_drm_device_t *device;
   int status;

   if (pipe == NULL)
      return 0;
   if (pipe->submit_count != 0 || pipe->fence_count != 0)
      return -EBUSY;
   device = pipe->device;
   status = qfd_pipe_del(pipe->transport);
   if (status != 0)
      return status;
   if (device->pipe_count != 0)
      device->pipe_count--;
   memset(pipe, 0, sizeof(*pipe));
   free(pipe);
   return 0;
}

int qfd_drm_pipe_get_param(qfd_drm_pipe_t *pipe, enum qfd_drm_param param,
                           uint64_t *out_value)
{
   uint32_t timestamp;
   int status;

   if (pipe == NULL || out_value == NULL)
      return -EINVAL;
   if (param != QFD_DRM_PARAM_TIMESTAMP)
      return qfd_drm_device_get_param(pipe->device, param, out_value);
   status = qfd_pipe_read_timestamp(pipe->transport, 2u, &timestamp);
   if (status == 0)
      *out_value = timestamp;
   return status;
}

int qfd_drm_bo_new(qfd_drm_device_t *device, uint32_t size,
                   qfd_drm_bo_t **out_bo)
{
   qfd_drm_bo_t *bo;
   int status;

   if (device == NULL || out_bo == NULL || size == 0)
      return -EINVAL;
   *out_bo = NULL;
   bo = (qfd_drm_bo_t *)calloc(1, sizeof(*bo));
   if (bo == NULL)
      return -ENOMEM;
   bo->device = device;
   status = qfd_bo_new(device->transport, size,
                       device->profile.allocation_flags, &bo->transport);
   if (status != 0)
   {
      free(bo);
      return status;
   }
   bo->refcount = 1;
   device->bo_count++;
   *out_bo = bo;
   return 0;
}

qfd_drm_bo_t *qfd_drm_bo_ref(qfd_drm_bo_t *bo)
{
   if (bo == NULL || bo->refcount == UINT32_MAX)
      return NULL;
   bo->refcount++;
   return bo;
}

int qfd_drm_bo_del(qfd_drm_bo_t *bo)
{
   qfd_drm_device_t *device;
   int status;

   if (bo == NULL)
      return 0;
   if (bo->refcount == 0)
      return -EINVAL;
   bo->refcount--;
   if (bo->refcount != 0)
      return 0;
   device = bo->device;
   status = qfd_bo_del(bo->transport);
   if (status != 0)
   {
      bo->refcount = 1;
      return status;
   }
   if (device->bo_count != 0)
      device->bo_count--;
   memset(bo, 0, sizeof(*bo));
   free(bo);
   return 0;
}

void *qfd_drm_bo_map(const qfd_drm_bo_t *bo)
{
   return bo != NULL ? qfd_bo_map(bo->transport) : NULL;
}

uint32_t qfd_drm_bo_size(const qfd_drm_bo_t *bo)
{
   return bo != NULL ? qfd_bo_size(bo->transport) : 0;
}

uint32_t qfd_drm_bo_iova(const qfd_drm_bo_t *bo)
{
   return bo != NULL ? qfd_bo_gpuaddr(bo->transport) : 0;
}

int qfd_drm_bo_cpu_prep(qfd_drm_bo_t *bo, uint32_t access)
{
   if (bo == NULL ||
       (access & ~(uint32_t)(QFD_DRM_BO_READ | QFD_DRM_BO_WRITE)) != 0 ||
       access == 0)
      return -EINVAL;
   if ((access & QFD_DRM_BO_READ) != 0)
      return qfd_bo_cache(bo->transport, 0, qfd_bo_size(bo->transport),
                          bo->device->profile.invalidate_operation);
   return 0;
}

static int qfd_drm_submit_attach(qfd_drm_submit_t *submit, qfd_drm_bo_t *bo,
                                 uint32_t access)
{
   uint32_t i;

   if (submit == NULL || bo == NULL || bo->device != submit->pipe->device ||
       (access & ~(uint32_t)(QFD_DRM_BO_READ | QFD_DRM_BO_WRITE)) != 0 ||
       access == 0)
      return -EINVAL;
   for (i = 0; i < submit->bo_count; ++i)
   {
      if (submit->bos[i].bo == bo)
      {
         submit->bos[i].access |= access;
         return 0;
      }
   }
   if (submit->bo_count == QFD_DRM_MAX_BOS || qfd_drm_bo_ref(bo) == NULL)
      return -ENOSPC;
   submit->bos[submit->bo_count].bo = bo;
   submit->bos[submit->bo_count].access = access;
   submit->bo_count++;
   return 0;
}

int qfd_drm_submit_new(qfd_drm_pipe_t *pipe, qfd_drm_submit_t **out_submit)
{
   qfd_drm_submit_t *submit;

   if (pipe == NULL || out_submit == NULL)
      return -EINVAL;
   *out_submit = NULL;
   submit = (qfd_drm_submit_t *)calloc(1, sizeof(*submit));
   if (submit == NULL)
      return -ENOMEM;
   submit->pipe = pipe;
   pipe->submit_count++;
   *out_submit = submit;
   return 0;
}

int qfd_drm_ring_new(qfd_drm_submit_t *submit, uint32_t size,
                     uint32_t flags, qfd_drm_ring_t **out_ring)
{
   qfd_drm_ring_t *ring;
   int status;

   if (submit == NULL || out_ring == NULL || size == 0 || submit->flushed ||
       (flags & ~(uint32_t)QFD_DRM_RING_PRIMARY) != 0)
      return -EINVAL;
   *out_ring = NULL;
   if (submit->ring_count == QFD_DRM_MAX_RINGS ||
       ((flags & QFD_DRM_RING_PRIMARY) != 0 && submit->primary != NULL))
      return -ENOSPC;
   ring = (qfd_drm_ring_t *)calloc(1, sizeof(*ring));
   if (ring == NULL)
      return -ENOMEM;
   ring->submit = submit;
   ring->capacity_dwords = (size + 3u) / 4u;
   ring->flags = flags;
   status = qfd_drm_bo_new(submit->pipe->device,
                           ring->capacity_dwords * 4u, &ring->bo);
   if (status != 0)
   {
      free(ring);
      return status;
   }
   ring->start = (uint32_t *)qfd_drm_bo_map(ring->bo);
   if (ring->start == NULL)
   {
      qfd_drm_bo_del(ring->bo);
      free(ring);
      return -ENOMEM;
   }
   submit->rings[submit->ring_count++] = ring;
   if ((flags & QFD_DRM_RING_PRIMARY) != 0)
      submit->primary = ring;
   *out_ring = ring;
   return 0;
}

int qfd_drm_ring_emit(qfd_drm_ring_t *ring, uint32_t value)
{
   if (ring == NULL || ring->submit->flushed)
      return -EINVAL;
   if (ring->used_dwords == ring->capacity_dwords)
      return -ENOSPC;
   ring->start[ring->used_dwords++] = value;
   return 0;
}

int qfd_drm_ring_emit_many(qfd_drm_ring_t *ring, const uint32_t *values,
                           uint32_t count)
{
   uint32_t i;
   int status;

   if (values == NULL && count != 0)
      return -EINVAL;
   for (i = 0; i < count; ++i)
   {
      status = qfd_drm_ring_emit(ring, values[i]);
      if (status != 0)
         return status;
   }
   return 0;
}

int qfd_drm_ring_reloc(qfd_drm_ring_t *ring, qfd_drm_bo_t *bo,
                       uint32_t offset, int32_t shift, uint32_t or_value,
                       uint32_t access)
{
   uint64_t address;
   int status;

   if (ring == NULL || bo == NULL || offset >= qfd_drm_bo_size(bo) ||
       shift < -31 || shift > 31)
      return -EINVAL;
   address = (uint64_t)qfd_drm_bo_iova(bo) + offset;
   if (shift < 0)
      address <<= (uint32_t)(-shift);
   else
      address >>= (uint32_t)shift;
   address |= or_value;
   if (address > UINT32_MAX)
      return -EOVERFLOW;
   status = qfd_drm_submit_attach(ring->submit, bo, access);
   if (status != 0)
      return status;
   return qfd_drm_ring_emit(ring, (uint32_t)address);
}

uint32_t qfd_drm_ring_dwords(const qfd_drm_ring_t *ring)
{
   return ring != NULL ? ring->used_dwords : 0;
}

int qfd_drm_submit_flush(qfd_drm_submit_t *submit,
                         qfd_drm_fence_t **out_fence)
{
   qfd_drm_fence_t *fence;
   qfd_ib_t ib;
   uint32_t timestamp = 0;
   uint32_t i;
   int status;

   if (submit == NULL || out_fence == NULL || submit->flushed ||
       submit->primary == NULL || submit->primary->used_dwords == 0)
      return -EINVAL;
   *out_fence = NULL;
   for (i = 0; i < submit->bo_count; ++i)
   {
      status = qfd_bo_cache(submit->bos[i].bo->transport, 0,
                            qfd_drm_bo_size(submit->bos[i].bo),
                            submit->pipe->device->profile.clean_operation);
      if (status != 0)
         return status;
   }
   for (i = 0; i < submit->ring_count; ++i)
   {
      status = qfd_bo_cache(submit->rings[i]->bo->transport, 0,
                            submit->rings[i]->used_dwords * 4u,
                            submit->pipe->device->profile.clean_operation);
      if (status != 0)
         return status;
   }

   fence = (qfd_drm_fence_t *)calloc(1, sizeof(*fence));
   if (fence == NULL)
      return -ENOMEM;
   fence->pipe = submit->pipe;
   fence->refcount = 1;
   for (i = 0; i < submit->bo_count; ++i)
   {
      if (qfd_drm_bo_ref(submit->bos[i].bo) == NULL)
      {
         qfd_drm_fence_del(fence);
         return -EOVERFLOW;
      }
      fence->bos[fence->bo_count++] = submit->bos[i];
   }

   ib.bo = submit->primary->bo->transport;
   ib.offset_bytes = 0;
   ib.size_dwords = submit->primary->used_dwords;
   status = qfd_pipe_submit(submit->pipe->transport, &ib, 1u,
                            submit->pipe->device->profile.submit_flags,
                            NULL, &timestamp);
   if (status != 0)
   {
      qfd_drm_fence_del(fence);
      return status;
   }
   fence->timestamp = timestamp;
   submit->pipe->fence_count++;
   submit->flushed = 1;
   *out_fence = fence;
   return 0;
}

qfd_drm_fence_t *qfd_drm_fence_ref(qfd_drm_fence_t *fence)
{
   if (fence == NULL || fence->refcount == UINT32_MAX)
      return NULL;
   fence->refcount++;
   return fence;
}

int qfd_drm_fence_wait(qfd_drm_fence_t *fence, uint32_t timeout_ms)
{
   uint32_t i;
   int status;

   if (fence == NULL)
      return -EINVAL;
   if (fence->waited)
      return 0;
   if (timeout_ms == 0)
      timeout_ms = fence->pipe->device->profile.wait_timeout_ms;
   status = qfd_pipe_wait_timestamp(fence->pipe->transport,
                                    fence->timestamp, timeout_ms);
   if (status != 0)
      return status;
   for (i = 0; i < fence->bo_count; ++i)
   {
      if ((fence->bos[i].access & QFD_DRM_BO_WRITE) == 0)
         continue;
      status = qfd_bo_cache(fence->bos[i].bo->transport, 0,
                            qfd_drm_bo_size(fence->bos[i].bo),
                            fence->pipe->device->profile.invalidate_operation);
      if (status != 0)
         return status;
   }
   fence->waited = 1;
   return 0;
}

uint32_t qfd_drm_fence_timestamp(const qfd_drm_fence_t *fence)
{
   return fence != NULL ? fence->timestamp : 0;
}

int qfd_drm_fence_del(qfd_drm_fence_t *fence)
{
   qfd_drm_pipe_t *pipe;
   uint32_t i;
   int result = 0;
   int status;

   if (fence == NULL)
      return 0;
   if (fence->refcount == 0)
      return -EINVAL;
   fence->refcount--;
   if (fence->refcount != 0)
      return 0;
   pipe = fence->pipe;
   for (i = 0; i < fence->bo_count; ++i)
   {
      status = qfd_drm_bo_del(fence->bos[i].bo);
      if (result == 0 && status != 0)
         result = status;
   }
   if (pipe->fence_count != 0)
      pipe->fence_count--;
   memset(fence, 0, sizeof(*fence));
   free(fence);
   return result;
}

int qfd_drm_submit_del(qfd_drm_submit_t *submit)
{
   qfd_drm_pipe_t *pipe;
   uint32_t i;
   int result = 0;
   int status;

   if (submit == NULL)
      return 0;
   pipe = submit->pipe;
   for (i = 0; i < submit->ring_count; ++i)
   {
      status = qfd_drm_bo_del(submit->rings[i]->bo);
      if (result == 0 && status != 0)
         result = status;
      free(submit->rings[i]);
   }
   for (i = 0; i < submit->bo_count; ++i)
   {
      status = qfd_drm_bo_del(submit->bos[i].bo);
      if (result == 0 && status != 0)
         result = status;
   }
   if (pipe->submit_count != 0)
      pipe->submit_count--;
   memset(submit, 0, sizeof(*submit));
   free(submit);
   return result;
}
