#include "qfd_winsys.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
   qfd_device_t *device = NULL;
   qfd_pipe_t *pipe = NULL;
   uint32_t timestamps[3] = {0, 0, 0};
   uint32_t type;
   int status;
   int failed = 0;

   puts("qfd_context_probe_mode=create_timestamp_destroy_no_submit");
   status = qfd_device_open(NULL, 0, 0, &device);
   printf("qfd_context_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   status = qfd_pipe_new(device, QFD_CONTEXT_TYPE_3D, 0, &pipe);
   printf("qfd_context_create_status=%d handle=%u\n", status,
          qfd_pipe_handle(pipe));
   if (status != 0)
   {
      failed = 1;
      goto close_device;
   }

   for (type = 1; type <= 3; ++type)
   {
      status = qfd_pipe_read_timestamp(pipe, type, &timestamps[type - 1]);
      printf("qfd_context_timestamp type=%u value=%u status=%d\n", type,
             timestamps[type - 1], status);
      failed |= status != 0;
   }

   status = qfd_pipe_wait_timestamp(pipe, timestamps[1], 1000u);
   printf("qfd_context_wait timestamp=%u status=%d\n", timestamps[1],
          status);
   failed |= status != 0;

   status = qfd_pipe_del(pipe);
   pipe = NULL;
   printf("qfd_context_destroy_status=%d\n", status);
   failed |= status != 0;

close_device:
   if (pipe != NULL)
      failed |= qfd_pipe_del(pipe) != 0;
   status = qfd_device_close(device);
   printf("qfd_context_device_close_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_context_probe: %s\n", failed ? "FAIL" : "PASS");
   return failed ? 1 : 0;
}
