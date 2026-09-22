#include "qfd_winsys.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A3xx type-3 packet: one-payload-dword CP_NOP (opcode 0x10).  This probe is
 * deliberately QEMU-only until flags and recovery are validated on hardware. */
#define QFD_A3XX_CP_NOP_HEADER 0xc0001000u

int main(int argc, char **argv)
{
   qfd_device_t *device = NULL;
   qfd_pipe_t *pipe = NULL;
   qfd_bo_t *bo = NULL;
   qfd_ib_t ib;
   uint32_t *map;
   uint32_t timestamp = 0;
   uint32_t retired = 0;
   uint32_t queued = 0;
   int status;
   int failed = 0;

   if (argc != 2 || strcmp(argv[1], "--qemu-only") != 0)
   {
      fputs("qfd_submit_probe: REFUSED (requires --qemu-only)\n", stderr);
      return 2;
   }

   puts("qfd_submit_probe_mode=qemu_only_cp_nop");
   status = qfd_device_open(NULL, 0, 0, &device);
   printf("qfd_submit_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   status = qfd_bo_new(device, 4096u, 0, &bo);
   printf("qfd_submit_alloc_status=%d\n", status);
   if (status != 0)
   {
      failed = 1;
      goto close_device;
   }
   map = (uint32_t *)qfd_bo_map(bo);
   if (map == NULL)
   {
      puts("qfd_submit_map_status=-1");
      failed = 1;
      goto free_bo;
   }
   map[0] = QFD_A3XX_CP_NOP_HEADER;
   map[1] = 0;
   status = qfd_bo_cache(bo, 0, 8u, QFD_CACHE_OPERATION_1);
   printf("qfd_submit_cache_status=%d gpuaddr=0x%08x\n", status,
          qfd_bo_gpuaddr(bo));
   if (status != 0)
   {
      failed = 1;
      goto free_bo;
   }

   status = qfd_pipe_new(device, QFD_CONTEXT_TYPE_3D, 0, &pipe);
   printf("qfd_submit_context_status=%d handle=%u\n", status,
          qfd_pipe_handle(pipe));
   if (status != 0)
   {
      failed = 1;
      goto free_bo;
   }

   ib.bo = bo;
   ib.offset_bytes = 0;
   ib.size_dwords = 2u;
   status = qfd_pipe_submit(pipe, &ib, 1u, 0, NULL, &timestamp);
   printf("qfd_submit_issue_status=%d timestamp=%u\n", status, timestamp);
   if (status != 0)
   {
      failed = 1;
      goto destroy_pipe;
   }

   status = qfd_pipe_wait_timestamp(pipe, timestamp, 5000u);
   printf("qfd_submit_wait_status=%d\n", status);
   failed |= status != 0;
   status = qfd_pipe_read_timestamp(pipe, 2u, &retired);
   printf("qfd_submit_retired_status=%d value=%u\n", status, retired);
   failed |= status != 0 || !qfd_timestamp_after(retired, timestamp - 1u);
   status = qfd_pipe_read_timestamp(pipe, 3u, &queued);
   printf("qfd_submit_queued_status=%d value=%u\n", status, queued);
   failed |= status != 0 || !qfd_timestamp_after(queued, timestamp - 1u);

destroy_pipe:
   status = qfd_pipe_del(pipe);
   pipe = NULL;
   printf("qfd_submit_context_destroy_status=%d\n", status);
   failed |= status != 0;
free_bo:
   status = qfd_bo_del(bo);
   bo = NULL;
   printf("qfd_submit_free_status=%d\n", status);
   failed |= status != 0;
close_device:
   if (pipe != NULL)
      failed |= qfd_pipe_del(pipe) != 0;
   if (bo != NULL)
      failed |= qfd_bo_del(bo) != 0;
   status = qfd_device_close(device);
   printf("qfd_submit_device_close_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_submit_probe: %s\n", failed ? "FAIL" : "PASS");
   return failed ? 1 : 0;
}
