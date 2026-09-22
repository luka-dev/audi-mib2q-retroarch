#include "qfd_winsys.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct mock_state {
   unsigned library_open_count;
   unsigned library_close_count;
   unsigned device_open_count;
   unsigned device_close_count;
   unsigned context_create_count;
   unsigned context_destroy_count;
   unsigned alloc_count;
   unsigned free_count;
   unsigned cache_count;
   unsigned submit_count;
   unsigned read_count;
   unsigned wait_count;
   int fail_getinfo;
   qnx_gsl_handle32_t context_handle;
   qnx_gsl_direct_ib32_t submitted_ib;
} mock_state_t;

static mock_state_t mock;

static int32_t mock_library_open(uint32_t flags)
{
   mock.library_open_count++;
   return flags == 0 ? 0 : -10;
}

static int32_t mock_library_close(void)
{
   mock.library_close_count++;
   return 0;
}

static const char *mock_library_version(void)
{
   return "mock-gsl-1";
}

static qnx_gsl_handle32_t mock_device_open(uint32_t device_id, uint32_t flags)
{
   mock.device_open_count++;
   return device_id == 1 && flags == 0 ? 0x11u : 0;
}

static int32_t mock_device_close(qnx_gsl_handle32_t device)
{
   mock.device_close_count++;
   return device == 0x11u ? 0 : -11;
}

static int32_t mock_device_getinfo(qnx_gsl_handle32_t device, void *info32)
{
   uint32_t *words = (uint32_t *)info32;
   unsigned i;

   if (mock.fail_getinfo)
      return -19;
   if (device != 0x11u)
      return -12;
   for (i = 0; i < QFD_DEVICE_INFO_WORDS; ++i)
      words[i] = 0xa3200000u + i;
   return 0;
}

static qnx_gsl_handle32_t mock_context_create(qnx_gsl_handle32_t device,
                                               uint32_t type,
                                               uint32_t flags)
{
   mock.context_create_count++;
   return device == 0x11u && type == QFD_CONTEXT_TYPE_3D && flags == 0x20u
                ? mock.context_handle
                : UINT32_MAX;
}

static int32_t mock_context_destroy(qnx_gsl_handle32_t device,
                                    qnx_gsl_handle32_t context)
{
   mock.context_destroy_count++;
   return device == 0x11u && context == mock.context_handle ? 0 : -13;
}

static int32_t mock_alloc(uint32_t size, uint32_t flags,
                          qnx_gsl_memdesc32_t *out)
{
   mock.alloc_count++;
   memset(out, 0, sizeof(*out));
   out->hostptr = 0x1000u;
   out->gpuaddr = 0x2000u;
   out->size = size;
   out->flags = flags;
   return 0;
}

static int32_t mock_free(qnx_gsl_memdesc32_t *memdesc)
{
   mock.free_count++;
   return memdesc->gpuaddr == 0x2000u ? 0 : -14;
}

static int32_t mock_cache(qnx_gsl_memdesc32_t *memdesc, uint32_t offset,
                          uint32_t size, uint32_t operation)
{
   mock.cache_count++;
   return memdesc->gpuaddr == 0x2000u && offset == 0x40u && size == 0x20u &&
                 operation == QFD_CACHE_OPERATION_1
                ? 0
                : -15;
}

static int32_t mock_submit(qnx_gsl_handle32_t device,
                           qnx_gsl_handle32_t context,
                           const qnx_gsl_direct_ib32_t *ibs,
                           uint32_t num_ibs, uint32_t *timestamp,
                           uint32_t flags, const void *syncobj)
{
   mock.submit_count++;
   if (device != 0x11u || context != 0x22u || num_ibs != 1 ||
       flags != 0x55u || syncobj != NULL)
      return -16;
   mock.submitted_ib = ibs[0];
   *timestamp = 77u;
   return 0;
}

static int32_t mock_read(qnx_gsl_handle32_t device,
                         qnx_gsl_handle32_t context, uint32_t type,
                         uint32_t *timestamp)
{
   mock.read_count++;
   if (device != 0x11u || context != 0x22u || type != 2u)
      return -17;
   *timestamp = 77u;
   return 0;
}

static int32_t mock_wait(qnx_gsl_handle32_t device,
                         qnx_gsl_handle32_t context, uint32_t timestamp,
                         uint32_t timeout)
{
   mock.wait_count++;
   return device == 0x11u && context == 0x22u && timestamp == 77u &&
                 timeout == 1000u
                ? 0
                : -18;
}

static qfd_gsl_dispatch_t mock_dispatch(void)
{
   qfd_gsl_dispatch_t dispatch;

   memset(&dispatch, 0, sizeof(dispatch));
   dispatch.library_open = mock_library_open;
   dispatch.library_close = mock_library_close;
   dispatch.library_version = mock_library_version;
   dispatch.device_open = mock_device_open;
   dispatch.device_close = mock_device_close;
   dispatch.device_getinfo = mock_device_getinfo;
   dispatch.context_create = mock_context_create;
   dispatch.context_destroy = mock_context_destroy;
   dispatch.memory_alloc_pure = mock_alloc;
   dispatch.memory_free_pure = mock_free;
   dispatch.memory_cacheoperation = mock_cache;
   dispatch.command_issueib_sync = mock_submit;
   dispatch.command_readtimestamp = mock_read;
   dispatch.command_waittimestamp = mock_wait;
   return dispatch;
}

#define CHECK(condition)                                                      \
   do                                                                         \
   {                                                                          \
      if (!(condition))                                                       \
      {                                                                       \
         fprintf(stderr, "selftest failure at line %d: %s\n", __LINE__,      \
                 #condition);                                                 \
         return 1;                                                            \
      }                                                                       \
   } while (0)

int main(void)
{
   qfd_gsl_dispatch_t dispatch = mock_dispatch();
   qfd_device_info_t info;
   qfd_device_t *device = NULL;
   qfd_pipe_t *pipe = NULL;
   qfd_bo_t *bo = NULL;
   qfd_ib_t ib;
   uint32_t timestamp = 0;

   memset(&mock, 0, sizeof(mock));
   mock.context_handle = 0x22u;
   CHECK(qfd_device_open("__qfd_library_that_does_not_exist__.so", 0, 0,
                         &device) == -ENOENT);
   CHECK(device == NULL);

   mock.fail_getinfo = 1;
   CHECK(qfd_device_open_dispatch(&dispatch, 0, 0, &device) == -19);
   CHECK(device == NULL);
   CHECK(mock.device_close_count == 1u);
   CHECK(mock.library_close_count == 1u);

   /* Context id zero is valid in the MU1316 ABI. */
   memset(&mock, 0, sizeof(mock));
   mock.context_handle = 0;
   CHECK(qfd_device_open_dispatch(&dispatch, 0, 0, &device) == 0);
   CHECK(qfd_pipe_new(device, QFD_CONTEXT_TYPE_3D, 0x20u, &pipe) == 0);
   CHECK(qfd_pipe_handle(pipe) == 0);
   CHECK(qfd_pipe_del(pipe) == 0);
   pipe = NULL;
   CHECK(qfd_device_close(device) == 0);
   device = NULL;

   memset(&mock, 0, sizeof(mock));
   mock.context_handle = 0x22u;
   CHECK(qfd_device_open_dispatch(&dispatch, 0, 0, &device) == 0);
   CHECK(device != NULL);
   CHECK(qfd_device_handle(device) == 0x11u);
   CHECK(strcmp(qfd_device_library_version(device), "mock-gsl-1") == 0);
   qfd_device_get_info(device, &info);
   CHECK(info.words[0] == 0xa3200000u);
   CHECK(info.words[7] == 0xa3200007u);

   CHECK(qfd_pipe_new(device, QFD_CONTEXT_TYPE_3D, 0x20u, &pipe) == 0);
   CHECK(qfd_pipe_handle(pipe) == 0x22u);
   CHECK(qfd_bo_new(device, 4096u, 0xc0300u, &bo) == 0);
   CHECK(qfd_bo_map(bo) == (void *)(uintptr_t)0x1000u);
   CHECK(qfd_bo_gpuaddr(bo) == 0x2000u);
   CHECK(qfd_bo_size(bo) == 4096u);
   CHECK(qfd_bo_flags(bo) == 0xc0300u);
   CHECK(qfd_device_close(device) == -EBUSY);

   CHECK(qfd_bo_cache(bo, 0x40u, 0x20u, QFD_CACHE_OPERATION_1) == 0);
   CHECK(qfd_bo_cache(bo, 4080u, 32u, QFD_CACHE_OPERATION_1) == -ERANGE);

   ib.bo = bo;
   ib.offset_bytes = 0x40u;
   ib.size_dwords = 4u;
   CHECK(qfd_pipe_submit(pipe, &ib, 1, 0x55u, NULL, &timestamp) == 0);
   CHECK(timestamp == 77u);
   CHECK(mock.submitted_ib.gpuaddr_lo == 0x2040u);
   CHECK(mock.submitted_ib.gpuaddr_hi_or_pad == 0);
   CHECK(mock.submitted_ib.sizedwords == 4u);
   CHECK(mock.submitted_ib.ctrl_or_reserved == 0);

   ib.offset_bytes = 4092u;
   ib.size_dwords = 2u;
   CHECK(qfd_pipe_submit(pipe, &ib, 1, 0, NULL, &timestamp) == -ERANGE);
   CHECK(mock.submit_count == 1u);

   CHECK(qfd_pipe_read_timestamp(pipe, 2u, &timestamp) == 0);
   CHECK(timestamp == 77u);
   CHECK(qfd_pipe_wait_timestamp(pipe, timestamp, 1000u) == 0);
   CHECK(qfd_timestamp_after(1u, UINT32_MAX));
   CHECK(!qfd_timestamp_after(UINT32_MAX, 1u));

   CHECK(qfd_bo_del(bo) == 0);
   CHECK(qfd_pipe_del(pipe) == 0);
   CHECK(qfd_device_close(device) == 0);

   CHECK(mock.library_open_count == 1u);
   CHECK(mock.library_close_count == 1u);
   CHECK(mock.device_open_count == 1u);
   CHECK(mock.device_close_count == 1u);
   CHECK(mock.context_create_count == 1u);
   CHECK(mock.context_destroy_count == 1u);
   CHECK(mock.alloc_count == 1u);
   CHECK(mock.free_count == 1u);
   CHECK(mock.cache_count == 1u);
   CHECK(mock.submit_count == 1u);
   CHECK(mock.read_count == 1u);
   CHECK(mock.wait_count == 1u);

   puts("qfd_winsys_selftest: PASS");
   return 0;
}

#undef CHECK
