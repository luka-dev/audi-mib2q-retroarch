#include "qfd_winsys.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define QFD_A3XX_CP_MEM_WRITE 0x3du
#define QFD_TYPE3_HEADER(opcode, payload_dwords)                              \
   (0xc0000000u | (((payload_dwords) - 1u) << 16) | ((opcode) << 8))

int main(int argc, char **argv)
{
   static const uint32_t guard = 0xfeedfaceu;
   static const uint32_t expected[] = {
      0xbf800000u, 0x3f800000u, 0x12345678u
   };
   qfd_device_t *device = NULL;
   qfd_pipe_t *pipe = NULL;
   qfd_bo_t *result_bo = NULL;
   qfd_bo_t *ib_bo = NULL;
   qfd_ib_t ib;
   uint32_t *result;
   uint32_t *commands;
   uint32_t timestamp = 0;
   uint32_t i;
   int status;
   int failed = 0;

   if (argc != 2 || strcmp(argv[1], "--qemu-only") != 0)
   {
      fputs("qfd_memwrite_probe: REFUSED (requires --qemu-only)\n", stderr);
      return 2;
   }

   puts("qfd_memwrite_probe_mode=qemu_only_cp_mem_write_readback");
   status = qfd_device_open(NULL, 0, 0, &device);
   printf("qfd_memwrite_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   status = qfd_bo_new(device, 4096u, 0, &result_bo);
   printf("qfd_memwrite_result_alloc_status=%d\n", status);
   if (status != 0)
   {
      failed = 1;
      goto close_device;
   }
   status = qfd_bo_new(device, 4096u, 0, &ib_bo);
   printf("qfd_memwrite_ib_alloc_status=%d\n", status);
   if (status != 0)
   {
      failed = 1;
      goto free_result;
   }

   result = (uint32_t *)qfd_bo_map(result_bo);
   commands = (uint32_t *)qfd_bo_map(ib_bo);
   if (result == NULL || commands == NULL)
   {
      printf("qfd_memwrite_map_status=-1 result=%p ib=%p\n",
             (void *)result, (void *)commands);
      failed = 1;
      goto free_ib;
   }

   for (i = 0; i < 5u; ++i)
      result[i] = guard;
   commands[0] = QFD_TYPE3_HEADER(QFD_A3XX_CP_MEM_WRITE, 4u);
   commands[1] = qfd_bo_gpuaddr(result_bo) + 4u;
   commands[2] = expected[0];
   commands[3] = expected[1];
   commands[4] = expected[2];

   status = qfd_bo_cache(result_bo, 0, 20u, QFD_CACHE_OPERATION_1);
   printf("qfd_memwrite_result_clean_status=%d\n", status);
   failed |= status != 0;
   status = qfd_bo_cache(ib_bo, 0, 20u, QFD_CACHE_OPERATION_1);
   printf("qfd_memwrite_ib_clean_status=%d\n", status);
   failed |= status != 0;
   if (failed)
      goto free_ib;

   status = qfd_pipe_new(device, QFD_CONTEXT_TYPE_3D, 0, &pipe);
   printf("qfd_memwrite_context_status=%d handle=%u\n", status,
          qfd_pipe_handle(pipe));
   if (status != 0)
   {
      failed = 1;
      goto free_ib;
   }

   ib.bo = ib_bo;
   ib.offset_bytes = 0;
   ib.size_dwords = 5u;
   status = qfd_pipe_submit(pipe, &ib, 1u, 0, NULL, &timestamp);
   printf("qfd_memwrite_issue_status=%d timestamp=%u dst=0x%08x\n",
          status, timestamp, commands[1]);
   if (status != 0)
   {
      failed = 1;
      goto destroy_pipe;
   }
   status = qfd_pipe_wait_timestamp(pipe, timestamp, 5000u);
   printf("qfd_memwrite_wait_status=%d\n", status);
   failed |= status != 0;
   status = qfd_bo_cache(result_bo, 0, 20u, QFD_CACHE_OPERATION_2);
   printf("qfd_memwrite_result_invalidate_status=%d\n", status);
   failed |= status != 0;

   printf("qfd_memwrite_readback=%08x,%08x,%08x,%08x,%08x\n",
          result[0], result[1], result[2], result[3], result[4]);
   failed |= result[0] != guard;
   failed |= result[1] != expected[0];
   failed |= result[2] != expected[1];
   failed |= result[3] != expected[2];
   failed |= result[4] != guard;

destroy_pipe:
   status = qfd_pipe_del(pipe);
   pipe = NULL;
   printf("qfd_memwrite_context_destroy_status=%d\n", status);
   failed |= status != 0;
free_ib:
   status = qfd_bo_del(ib_bo);
   ib_bo = NULL;
   printf("qfd_memwrite_ib_free_status=%d\n", status);
   failed |= status != 0;
free_result:
   status = qfd_bo_del(result_bo);
   result_bo = NULL;
   printf("qfd_memwrite_result_free_status=%d\n", status);
   failed |= status != 0;
close_device:
   if (pipe != NULL)
      failed |= qfd_pipe_del(pipe) != 0;
   if (ib_bo != NULL)
      failed |= qfd_bo_del(ib_bo) != 0;
   if (result_bo != NULL)
      failed |= qfd_bo_del(result_bo) != 0;
   status = qfd_device_close(device);
   printf("qfd_memwrite_device_close_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_memwrite_probe: %s\n", failed ? "FAIL" : "PASS");
   return failed ? 1 : 0;
}
