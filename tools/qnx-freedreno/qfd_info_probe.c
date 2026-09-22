/* Safe production-dispatch probe: library/device open + getinfo only. */

#include "qfd_winsys.h"

#include <inttypes.h>
#include <stdio.h>

int main(void)
{
   qfd_device_info_t info;
   qfd_device_t *device = NULL;
   const char *version;
   unsigned i;
   int status;

   puts("qfd_probe_mode=open_getinfo_only");
   status = qfd_device_open("libGSLUser.so", 0, 0, &device);
   printf("qfd_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   version = qfd_device_library_version(device);
   printf("gsl_library_version=%s\n", version != NULL ? version : "(null)");
   printf("gsl_device_handle=0x%08" PRIx32 "\n", qfd_device_handle(device));
   qfd_device_get_info(device, &info);
   for (i = 0; i < QFD_DEVICE_INFO_WORDS; ++i)
      printf("device_info_%02x=0x%08" PRIx32 "\n", i * 4, info.words[i]);

   status = qfd_device_close(device);
   printf("qfd_device_close_status=%d\n", status);
   return status == 0 ? 0 : 1;
}
