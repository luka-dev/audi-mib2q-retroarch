#include "qfd_drmif_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define QFD_A3XX_CP_MEM_WRITE 0x3du
#define QFD_TYPE3_HEADER(opcode, payload_dwords)                              \
   (0xc0000000u | (((payload_dwords) - 1u) << 16) | ((opcode) << 8))

int main(int argc, char **argv)
{
   static const uint32_t guard = 0xfeedfaceu;
   qfd_drm_profile_t profile;
   qfd_drm_device_t *device = NULL;
   qfd_drm_pipe_t *pipe = NULL;
   qfd_drm_bo_t *result_bo = NULL;
   qfd_drm_submit_t *submit = NULL;
   qfd_drm_ring_t *ring = NULL;
   qfd_drm_fence_t *fence = NULL;
   uint32_t *result;
   uint64_t value;
   int status;
   int failed = 0;

   if (argc != 2 || strcmp(argv[1], "--qemu-only") != 0)
   {
      fputs("qfd_drmif_probe: REFUSED (requires --qemu-only)\n", stderr);
      return 2;
   }

   puts("qfd_drmif_probe_mode=qemu_only_object_model_reloc_readback");
   qfd_drm_profile_qemu(&profile);
   status = qfd_drm_device_open(NULL, &profile, &device);
   printf("qfd_drmif_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   status = qfd_drm_device_get_param(device, QFD_DRM_PARAM_GPU_ID, &value);
   printf("qfd_drmif_gpu_id_status=%d value=%llu\n", status,
          (unsigned long long)value);
   failed |= status != 0 || value != 320u;
   status = qfd_drm_device_get_param(device, QFD_DRM_PARAM_CHIP_ID, &value);
   printf("qfd_drmif_chip_id_status=%d value=0x%08llx\n", status,
          (unsigned long long)value);
   failed |= status != 0 || value != 0x03020000u;
   status = qfd_drm_device_get_param(device, QFD_DRM_PARAM_GMEM_SIZE, &value);
   printf("qfd_drmif_gmem_status=%d value=%llu\n", status,
          (unsigned long long)value);
   failed |= status != 0 || value != 0x80000u;
   if (failed)
      goto close_device;

   status = qfd_drm_pipe_new(device, &pipe);
   printf("qfd_drmif_pipe_status=%d\n", status);
   if (status != 0)
   {
      failed = 1;
      goto close_device;
   }
   status = qfd_drm_bo_new(device, 4096u, &result_bo);
   printf("qfd_drmif_result_bo_status=%d iova=0x%08x\n", status,
          qfd_drm_bo_iova(result_bo));
   if (status != 0)
   {
      failed = 1;
      goto destroy_pipe;
   }
   result = (uint32_t *)qfd_drm_bo_map(result_bo);
   if (result == NULL)
   {
      failed = 1;
      goto free_result;
   }
   result[0] = guard;
   result[1] = guard;
   result[2] = guard;

   status = qfd_drm_submit_new(pipe, &submit);
   status |= qfd_drm_ring_new(submit, 4096u, QFD_DRM_RING_PRIMARY, &ring);
   status |= qfd_drm_ring_emit(ring,
      QFD_TYPE3_HEADER(QFD_A3XX_CP_MEM_WRITE, 2u));
   status |= qfd_drm_ring_reloc(ring, result_bo, 4u, 0, 0,
                                QFD_DRM_BO_WRITE);
   status |= qfd_drm_ring_emit(ring, 0x1234abcdu);
   printf("qfd_drmif_ring_status=%d dwords=%u\n", status,
          qfd_drm_ring_dwords(ring));
   if (status != 0)
   {
      failed = 1;
      goto destroy_submit;
   }

   status = qfd_drm_submit_flush(submit, &fence);
   printf("qfd_drmif_flush_status=%d timestamp=%u\n", status,
          qfd_drm_fence_timestamp(fence));
   if (status != 0)
   {
      failed = 1;
      goto destroy_submit;
   }
   status = qfd_drm_fence_wait(fence, 0);
   printf("qfd_drmif_wait_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_drmif_readback=%08x,%08x,%08x\n",
          result[0], result[1], result[2]);
   failed |= result[0] != guard || result[1] != 0x1234abcdu ||
             result[2] != guard;

destroy_submit:
   status = qfd_drm_fence_del(fence);
   fence = NULL;
   failed |= status != 0;
   status = qfd_drm_submit_del(submit);
   submit = NULL;
   failed |= status != 0;
free_result:
   status = qfd_drm_bo_del(result_bo);
   result_bo = NULL;
   failed |= status != 0;
destroy_pipe:
   status = qfd_drm_pipe_del(pipe);
   pipe = NULL;
   failed |= status != 0;
close_device:
   if (fence != NULL)
      failed |= qfd_drm_fence_del(fence) != 0;
   if (submit != NULL)
      failed |= qfd_drm_submit_del(submit) != 0;
   if (result_bo != NULL)
      failed |= qfd_drm_bo_del(result_bo) != 0;
   if (pipe != NULL)
      failed |= qfd_drm_pipe_del(pipe) != 0;
   status = qfd_drm_device_close(device);
   printf("qfd_drmif_device_close_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_drmif_probe: %s\n", failed ? "FAIL" : "PASS");
   return failed ? 1 : 0;
}
