#include "qfd_winsys.h"

#include <stdint.h>
#include <stdio.h>

static uint8_t qfd_pattern(uint32_t offset, uint32_t salt)
{
   uint32_t value = offset * 33u + salt * 17u;
   value ^= value >> 11;
   return (uint8_t)value;
}

static int qfd_verify_pattern(const uint8_t *map, uint32_t size,
                              uint32_t salt)
{
   uint32_t i;

   for (i = 0; i < size; ++i)
      if (map[i] != qfd_pattern(i, salt))
      {
         fprintf(stderr,
                 "qfd_mem_pattern_mismatch offset=%u expected=0x%02x got=0x%02x\n",
                 i, (unsigned)qfd_pattern(i, salt), (unsigned)map[i]);
         return 1;
      }
   return 0;
}

static int qfd_exercise_bo(qfd_device_t *device, uint32_t requested_size,
                           uint32_t allocation_flags)
{
   qfd_bo_t *bo = NULL;
   uint8_t *map;
   uint32_t allocated_size;
   uint32_t i;
   int status;
   int failed = 0;

   status = qfd_bo_new(device, requested_size, allocation_flags, &bo);
   printf("qfd_mem_alloc size=%u flags=0x%08x status=%d\n",
          requested_size, allocation_flags, status);
   if (status != 0)
      return 1;

   map = (uint8_t *)qfd_bo_map(bo);
   allocated_size = qfd_bo_size(bo);
   printf("qfd_mem_desc requested=%u allocated=%u gpuaddr=0x%08x "
          "hostptr=%p returned_flags=0x%llx\n",
          requested_size, allocated_size, qfd_bo_gpuaddr(bo), (void *)map,
          (unsigned long long)qfd_bo_flags(bo));
   if (map == NULL || allocated_size < requested_size)
      failed = 1;

   if (!failed)
   {
      for (i = 0; i < requested_size; ++i)
         map[i] = qfd_pattern(i, requested_size);
      status = qfd_bo_cache(bo, 0, requested_size, QFD_CACHE_OPERATION_1);
      printf("qfd_mem_cache op=1 size=%u status=%d\n", requested_size,
             status);
      failed |= status != 0;
      failed |= qfd_verify_pattern(map, requested_size, requested_size);

      for (i = 0; i < requested_size; ++i)
         map[i] = qfd_pattern(i, requested_size ^ 0xa5a5u);
      status = qfd_bo_cache(bo, 0, requested_size, QFD_CACHE_OPERATION_2);
      printf("qfd_mem_cache op=2 size=%u status=%d\n", requested_size,
             status);
      failed |= status != 0;
      failed |= qfd_verify_pattern(map, requested_size,
                                   requested_size ^ 0xa5a5u);
   }

   status = qfd_bo_del(bo);
   printf("qfd_mem_free size=%u status=%d\n", requested_size, status);
   failed |= status != 0;
   return failed;
}

int main(void)
{
   static const uint32_t sizes[] = {4096u, 65536u, 1048576u};
   qfd_device_t *device = NULL;
   uint32_t i;
   int status;
   int failed = 0;

   puts("qfd_mem_probe_mode=qemu_lifecycle_no_gpu_commands");
   status = qfd_device_open(NULL, 0, 0, &device);
   printf("qfd_mem_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   for (i = 0; i < (uint32_t)(sizeof(sizes) / sizeof(sizes[0])); ++i)
      failed |= qfd_exercise_bo(device, sizes[i], 0);

   status = qfd_device_close(device);
   printf("qfd_mem_device_close_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_mem_probe: %s\n", failed ? "FAIL" : "PASS");
   return failed ? 1 : 0;
}
