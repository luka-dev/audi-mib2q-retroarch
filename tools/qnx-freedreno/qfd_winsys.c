#include "qfd_winsys.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct qfd_device {
   qfd_gsl_dispatch_t gsl;
   void *library;
   qnx_gsl_handle32_t handle;
   qfd_device_info_t info;
   const char *library_version;
   uint32_t pipe_count;
   uint32_t bo_count;
   int library_is_open;
};

struct qfd_pipe {
   qfd_device_t *device;
   qnx_gsl_handle32_t handle;
};

struct qfd_bo {
   qfd_device_t *device;
   qnx_gsl_memdesc32_t memdesc;
};

static int qfd_dispatch_is_complete(const qfd_gsl_dispatch_t *gsl)
{
   return gsl != NULL && gsl->library_open != NULL &&
          gsl->library_close != NULL && gsl->library_version != NULL &&
          gsl->device_open != NULL && gsl->device_close != NULL &&
          gsl->device_getinfo != NULL && gsl->context_create != NULL &&
          gsl->context_destroy != NULL && gsl->memory_alloc_pure != NULL &&
          gsl->memory_free_pure != NULL &&
          gsl->memory_cacheoperation != NULL &&
          gsl->command_issueib_sync != NULL &&
          gsl->command_readtimestamp != NULL &&
          gsl->command_waittimestamp != NULL;
}

static int qfd_load_symbol(void *library, const char *name, void *out,
                           size_t out_size)
{
   const char *error;
   void *symbol;

   if (out_size != sizeof(symbol))
      return -EINVAL;

   dlerror();
   symbol = dlsym(library, name);
   error = dlerror();
   if (error != NULL || symbol == NULL)
      return -ENOENT;

   memcpy(out, &symbol, sizeof(symbol));
   return 0;
}

#define QFD_LOAD(gsl, library, field, name)                                  \
   do                                                                         \
   {                                                                          \
      int load_status = qfd_load_symbol((library), (name), &(gsl)->field,     \
                                        sizeof((gsl)->field));                \
      if (load_status != 0)                                                   \
         return load_status;                                                  \
   } while (0)

static int qfd_load_dispatch(void *library, qfd_gsl_dispatch_t *gsl)
{
   memset(gsl, 0, sizeof(*gsl));
   QFD_LOAD(gsl, library, library_open, "gsl_library_open");
   QFD_LOAD(gsl, library, library_close, "gsl_library_close");
   QFD_LOAD(gsl, library, library_version, "gsl_library_version");
   QFD_LOAD(gsl, library, device_open, "gsl_device_open");
   QFD_LOAD(gsl, library, device_close, "gsl_device_close");
   QFD_LOAD(gsl, library, device_getinfo, "gsl_device_getinfo");
   QFD_LOAD(gsl, library, context_create, "gsl_context_create");
   QFD_LOAD(gsl, library, context_destroy, "gsl_context_destroy");
   QFD_LOAD(gsl, library, memory_alloc_pure, "gsl_memory_alloc_pure");
   QFD_LOAD(gsl, library, memory_free_pure, "gsl_memory_free_pure");
   QFD_LOAD(gsl, library, memory_cacheoperation,
            "gsl_memory_cacheoperation");
   QFD_LOAD(gsl, library, command_issueib_sync,
            "gsl_command_issueib_sync");
   QFD_LOAD(gsl, library, command_readtimestamp,
            "gsl_command_readtimestamp");
   QFD_LOAD(gsl, library, command_waittimestamp,
            "gsl_command_waittimestamp");
   return 0;
}

#undef QFD_LOAD

int qfd_device_open_dispatch(const qfd_gsl_dispatch_t *dispatch,
                             uint32_t library_flags,
                             uint32_t device_flags,
                             qfd_device_t **out_device)
{
   qfd_device_t *device;
   int status;

   if (out_device == NULL)
      return -EINVAL;
   *out_device = NULL;
   if (!qfd_dispatch_is_complete(dispatch))
      return -EINVAL;

   device = (qfd_device_t *)calloc(1, sizeof(*device));
   if (device == NULL)
      return -ENOMEM;
   device->gsl = *dispatch;

   status = device->gsl.library_open(library_flags);
   if (status != 0)
   {
      free(device);
      return status;
   }
   device->library_is_open = 1;
   device->library_version = device->gsl.library_version();

   device->handle = device->gsl.device_open(1, device_flags);
   if (device->handle == 0 || device->handle == UINT32_MAX)
   {
      device->gsl.library_close();
      free(device);
      return -ENODEV;
   }

   status = device->gsl.device_getinfo(device->handle, device->info.words);
   if (status != 0)
   {
      device->gsl.device_close(device->handle);
      device->gsl.library_close();
      free(device);
      return status;
   }

   *out_device = device;
   return 0;
}

int qfd_device_open(const char *library_path, uint32_t library_flags,
                    uint32_t device_flags, qfd_device_t **out_device)
{
   qfd_gsl_dispatch_t dispatch;
   qfd_device_t *device;
   void *library;
   int status;

   if (out_device == NULL)
      return -EINVAL;
   *out_device = NULL;

   if (library_path == NULL)
      library_path = "libGSLUser.so";
   library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
   if (library == NULL)
      return -ENOENT;

   status = qfd_load_dispatch(library, &dispatch);
   if (status != 0)
   {
      dlclose(library);
      return status;
   }

   status = qfd_device_open_dispatch(&dispatch, library_flags, device_flags,
                                     &device);
   if (status != 0)
   {
      dlclose(library);
      return status;
   }

   device->library = library;
   *out_device = device;
   return 0;
}

int qfd_device_close(qfd_device_t *device)
{
   void *library;
   int result = 0;
   int status;

   if (device == NULL)
      return 0;
   if (device->pipe_count != 0 || device->bo_count != 0)
      return -EBUSY;

   library = device->library;
   if (device->handle != 0 && device->handle != UINT32_MAX)
   {
      status = device->gsl.device_close(device->handle);
      if (status != 0)
         result = status;
   }
   if (device->library_is_open)
   {
      status = device->gsl.library_close();
      if (result == 0 && status != 0)
         result = status;
   }

   memset(device, 0, sizeof(*device));
   free(device);
   if (library != NULL)
      dlclose(library);
   return result;
}

const char *qfd_device_library_version(const qfd_device_t *device)
{
   return device != NULL ? device->library_version : NULL;
}

qnx_gsl_handle32_t qfd_device_handle(const qfd_device_t *device)
{
   return device != NULL ? device->handle : 0;
}

void qfd_device_get_info(const qfd_device_t *device,
                         qfd_device_info_t *out_info)
{
   if (out_info == NULL)
      return;
   if (device == NULL)
      memset(out_info, 0, sizeof(*out_info));
   else
      *out_info = device->info;
}

int qfd_pipe_new(qfd_device_t *device, uint32_t context_type,
                 uint32_t context_flags, qfd_pipe_t **out_pipe)
{
   qfd_pipe_t *pipe;

   if (out_pipe == NULL)
      return -EINVAL;
   *out_pipe = NULL;
   if (device == NULL)
      return -EINVAL;

   pipe = (qfd_pipe_t *)calloc(1, sizeof(*pipe));
   if (pipe == NULL)
      return -ENOMEM;
   pipe->device = device;
   pipe->handle = device->gsl.context_create(device->handle, context_type,
                                             context_flags);
   /* MU1316 OpenGLES20 accepts context 0 and rejects only 0xffffffff
    * (stock call site 0x9f40c..0x9f420).  The QEMU server also deliberately
    * assigns context ids starting at zero. */
   if (pipe->handle == UINT32_MAX)
   {
      free(pipe);
      return -ENODEV;
   }

   device->pipe_count++;
   *out_pipe = pipe;
   return 0;
}

int qfd_pipe_del(qfd_pipe_t *pipe)
{
   qfd_device_t *device;
   int status;

   if (pipe == NULL)
      return 0;
   device = pipe->device;
   status = device->gsl.context_destroy(device->handle, pipe->handle);
   if (device->pipe_count != 0)
      device->pipe_count--;
   memset(pipe, 0, sizeof(*pipe));
   free(pipe);
   return status;
}

qnx_gsl_handle32_t qfd_pipe_handle(const qfd_pipe_t *pipe)
{
   return pipe != NULL ? pipe->handle : 0;
}

int qfd_bo_new(qfd_device_t *device, uint32_t size, uint32_t allocation_flags,
               qfd_bo_t **out_bo)
{
   qfd_bo_t *bo;
   int status;

   if (out_bo == NULL)
      return -EINVAL;
   *out_bo = NULL;
   if (device == NULL || size == 0)
      return -EINVAL;

   bo = (qfd_bo_t *)calloc(1, sizeof(*bo));
   if (bo == NULL)
      return -ENOMEM;
   bo->device = device;

   status = device->gsl.memory_alloc_pure(size, allocation_flags,
                                          &bo->memdesc);
   if (status != 0)
   {
      free(bo);
      return status;
   }

   if ((bo->memdesc.gpuaddr >> 32) != 0 || bo->memdesc.gpuaddr == 0 ||
       bo->memdesc.size < size || bo->memdesc.size > UINT32_MAX)
   {
      device->gsl.memory_free_pure(&bo->memdesc);
      free(bo);
      return -EOVERFLOW;
   }

   device->bo_count++;
   *out_bo = bo;
   return 0;
}

int qfd_bo_del(qfd_bo_t *bo)
{
   qfd_device_t *device;
   int status;

   if (bo == NULL)
      return 0;
   device = bo->device;
   status = device->gsl.memory_free_pure(&bo->memdesc);
   if (device->bo_count != 0)
      device->bo_count--;
   memset(bo, 0, sizeof(*bo));
   free(bo);
   return status;
}

void *qfd_bo_map(const qfd_bo_t *bo)
{
   return bo != NULL ? (void *)(uintptr_t)bo->memdesc.hostptr : NULL;
}

uint32_t qfd_bo_gpuaddr(const qfd_bo_t *bo)
{
   return bo != NULL ? (uint32_t)bo->memdesc.gpuaddr : 0;
}

uint32_t qfd_bo_size(const qfd_bo_t *bo)
{
   return bo != NULL ? (uint32_t)bo->memdesc.size : 0;
}

uint64_t qfd_bo_flags(const qfd_bo_t *bo)
{
   return bo != NULL ? bo->memdesc.flags : 0;
}

static int qfd_range_valid(const qfd_bo_t *bo, uint32_t offset,
                           uint32_t size)
{
   uint64_t end;

   if (bo == NULL || size == 0)
      return 0;
   end = (uint64_t)offset + size;
   return end <= bo->memdesc.size;
}

int qfd_bo_cache(qfd_bo_t *bo, uint32_t offset_bytes, uint32_t size_bytes,
                 uint32_t operation)
{
   if (!qfd_range_valid(bo, offset_bytes, size_bytes))
      return -ERANGE;
   if (operation != QFD_CACHE_OPERATION_1 &&
       operation != QFD_CACHE_OPERATION_2)
      return -EINVAL;
   return bo->device->gsl.memory_cacheoperation(&bo->memdesc, offset_bytes,
                                                size_bytes, operation);
}

int qfd_pipe_submit(qfd_pipe_t *pipe, const qfd_ib_t *ibs, uint32_t num_ibs,
                    uint32_t submit_flags, const void *syncobj,
                    uint32_t *inout_timestamp)
{
   qnx_gsl_direct_ib32_t direct[QFD_MAX_SUBMIT_IBS];
   uint32_t i;

   if (pipe == NULL || ibs == NULL || inout_timestamp == NULL ||
       num_ibs == 0 || num_ibs > QFD_MAX_SUBMIT_IBS)
      return -EINVAL;

   memset(direct, 0, sizeof(direct));
   for (i = 0; i < num_ibs; ++i)
   {
      uint64_t size_bytes = (uint64_t)ibs[i].size_dwords * 4u;
      uint64_t gpuaddr;

      if (ibs[i].bo == NULL || ibs[i].bo->device != pipe->device ||
          (ibs[i].offset_bytes & 3u) != 0 || size_bytes == 0 ||
          size_bytes > UINT32_MAX ||
          !qfd_range_valid(ibs[i].bo, ibs[i].offset_bytes,
                           (uint32_t)size_bytes))
         return -ERANGE;

      gpuaddr = ibs[i].bo->memdesc.gpuaddr + ibs[i].offset_bytes;
      if (gpuaddr > UINT32_MAX)
         return -EOVERFLOW;

      direct[i].gpuaddr_lo = (uint32_t)gpuaddr;
      direct[i].gpuaddr_hi_or_pad = 0;
      direct[i].sizedwords = ibs[i].size_dwords;
      direct[i].ctrl_or_reserved = 0;
   }

   return pipe->device->gsl.command_issueib_sync(
         pipe->device->handle, pipe->handle, direct, num_ibs,
         inout_timestamp, submit_flags, syncobj);
}

int qfd_pipe_read_timestamp(qfd_pipe_t *pipe, uint32_t timestamp_type,
                            uint32_t *out_timestamp)
{
   if (pipe == NULL || out_timestamp == NULL)
      return -EINVAL;
   return pipe->device->gsl.command_readtimestamp(
         pipe->device->handle, pipe->handle, timestamp_type, out_timestamp);
}

int qfd_pipe_wait_timestamp(qfd_pipe_t *pipe, uint32_t timestamp,
                            uint32_t timeout)
{
   if (pipe == NULL)
      return -EINVAL;
   return pipe->device->gsl.command_waittimestamp(
         pipe->device->handle, pipe->handle, timestamp, timeout);
}

int qfd_timestamp_after(uint32_t a, uint32_t b)
{
   return (int32_t)(a - b) > 0;
}
