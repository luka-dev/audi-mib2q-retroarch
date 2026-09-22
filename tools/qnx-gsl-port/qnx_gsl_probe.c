/*
 * Non-submitting MU1316 GSL ABI probe.
 *
 * This intentionally stops before memory allocation, context creation, cache
 * operations, or command submission.  Its purpose is to validate that the
 * recovered public ABI can load the exact on-device libGSLUser, enter the
 * driver, open device 1, and fetch its 32-byte info record.
 */

#include "qnx_gsl_abi.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gsl_api {
   void *library;
   qnx_gsl_library_open_fn library_open;
   qnx_gsl_library_close_fn library_close;
   qnx_gsl_library_version_fn library_version;
   qnx_gsl_device_open_fn device_open;
   qnx_gsl_device_close_fn device_close;
   qnx_gsl_device_getinfo_fn device_getinfo;
} gsl_api_t;

static int load_function(void *library, const char *name, void *out,
                         size_t out_size)
{
   void *symbol;
   const char *error;

   dlerror();
   symbol = dlsym(library, name);
   error = dlerror();
   if (error != NULL || symbol == NULL)
   {
      fprintf(stderr, "qnx_gsl_probe: dlsym(%s): %s\n", name,
              error != NULL ? error : "symbol is null");
      return -1;
   }
   if (out_size != sizeof(symbol))
   {
      fprintf(stderr, "qnx_gsl_probe: unexpected function pointer size\n");
      return -1;
   }
   memcpy(out, &symbol, sizeof(symbol));
   return 0;
}

#define LOAD_REQUIRED(api, field, symbol_name)                               \
   do                                                                         \
   {                                                                          \
      if (load_function((api)->library, symbol_name, &(api)->field,           \
                        sizeof((api)->field)) != 0)                            \
         return -1;                                                           \
   } while (0)

static int load_api(gsl_api_t *api, const char *path)
{
   memset(api, 0, sizeof(*api));
   api->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (api->library == NULL)
   {
      fprintf(stderr, "qnx_gsl_probe: dlopen(%s): %s\n", path, dlerror());
      return -1;
   }

   LOAD_REQUIRED(api, library_open, "gsl_library_open");
   LOAD_REQUIRED(api, library_close, "gsl_library_close");
   LOAD_REQUIRED(api, library_version, "gsl_library_version");
   LOAD_REQUIRED(api, device_open, "gsl_device_open");
   LOAD_REQUIRED(api, device_close, "gsl_device_close");
   LOAD_REQUIRED(api, device_getinfo, "gsl_device_getinfo");
   return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
   char *end = NULL;
   unsigned long parsed;

   errno = 0;
   parsed = strtoul(text, &end, 0);
   if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
      return -1;
   *value = (uint32_t)parsed;
   return 0;
}

static void print_usage(const char *argv0)
{
   fprintf(stderr,
           "usage: %s [--lib PATH] [--library-flags N] [--device-flags N]\n"
           "       no context, allocation, cache operation, or IB is issued\n",
           argv0);
}

int main(int argc, char **argv)
{
   const char *library_path = "libGSLUser.so";
   uint32_t library_flags = 0;
   uint32_t device_flags = 0;
   uint32_t info[8];
   qnx_gsl_handle32_t device = 0;
   gsl_api_t api;
   int library_is_open = 0;
   int device_is_open = 0;
   int result = 1;
   int status;
   int i;

   for (i = 1; i < argc; ++i)
   {
      if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc)
         library_path = argv[++i];
      else if (strcmp(argv[i], "--library-flags") == 0 && i + 1 < argc)
      {
         if (parse_u32(argv[++i], &library_flags) != 0)
         {
            print_usage(argv[0]);
            return 2;
         }
      }
      else if (strcmp(argv[i], "--device-flags") == 0 && i + 1 < argc)
      {
         if (parse_u32(argv[++i], &device_flags) != 0)
         {
            print_usage(argv[0]);
            return 2;
         }
      }
      else
      {
         print_usage(argv[0]);
         return 2;
      }
   }

   printf("probe_mode=open_getinfo_only\n");
   printf("library_path=%s\n", library_path);
   printf("library_flags=0x%08" PRIx32 "\n", library_flags);
   printf("device_id=1\n");
   printf("device_flags=0x%08" PRIx32 "\n", device_flags);

   if (load_api(&api, library_path) != 0)
      goto cleanup;

   printf("gsl_library_version=%s\n", api.library_version());
   status = api.library_open(library_flags);
   printf("gsl_library_open_status=%d\n", status);
   if (status != 0)
      goto cleanup;
   library_is_open = 1;

   device = api.device_open(1, device_flags);
   printf("gsl_device_handle=0x%08" PRIx32 "\n", device);
   if (device == 0 || device == UINT32_MAX)
   {
      fprintf(stderr, "qnx_gsl_probe: gsl_device_open failed\n");
      goto cleanup;
   }
   device_is_open = 1;

   memset(info, 0, sizeof(info));
   status = api.device_getinfo(device, info);
   printf("gsl_device_getinfo_status=%d\n", status);
   if (status != 0)
      goto cleanup;

   for (i = 0; i < 8; ++i)
      printf("device_info_%02x=0x%08" PRIx32 "\n", i * 4, info[i]);

   result = 0;

cleanup:
   if (device_is_open)
   {
      status = api.device_close(device);
      printf("gsl_device_close_status=%d\n", status);
      if (status != 0)
         result = 1;
   }
   if (library_is_open)
   {
      status = api.library_close();
      printf("gsl_library_close_status=%d\n", status);
      if (status != 0)
         result = 1;
   }
   if (api.library != NULL)
      dlclose(api.library);
   return result;
}

#undef LOAD_REQUIRED
