#include "qnx_priv.h"

static int
qnx_bo_offset(struct fd_bo *bo, uint64_t *offset)
{
   *offset = qfd_bo_gpuaddr(to_qnx_bo(bo)->transport);
   return 0;
}

static void *
qnx_bo_map(struct fd_bo *bo)
{
   return qfd_bo_map(to_qnx_bo(bo)->transport);
}

static int
qnx_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   struct qnx_bo *qbo = to_qnx_bo(bo);

   if (op & FD_BO_PREP_READ)
      return qfd_bo_cache(qbo->transport, 0, bo->size,
                          QFD_CACHE_OPERATION_2);
   return 0;
}

static int
qnx_bo_madvise(struct fd_bo *bo, int willneed)
{
   return willneed;
}

static uint64_t
qnx_bo_iova(struct fd_bo *bo)
{
   return qfd_bo_gpuaddr(to_qnx_bo(bo)->transport);
}

static void
qnx_bo_set_name(struct fd_bo *bo, const char *fmt, va_list ap)
{
}

static int
qnx_bo_dmabuf(struct fd_bo *bo)
{
   return -ENOSYS;
}

static void
qnx_bo_destroy(struct fd_bo *bo)
{
   struct qnx_bo *qbo = to_qnx_bo(bo);
   struct fd_device *dev = bo->dev;
   uint32_t handle = bo->handle;

   fd_bo_fini_fences(bo);
   simple_mtx_lock(&table_lock);
   _mesa_hash_table_remove_key(dev->handle_table, &handle);
   simple_mtx_unlock(&table_lock);
   qfd_bo_del(qbo->transport);
   free(qbo);
}

static const struct fd_bo_funcs qnx_bo_funcs = {
   .offset = qnx_bo_offset,
   .map = qnx_bo_map,
   .cpu_prep = qnx_bo_cpu_prep,
   .madvise = qnx_bo_madvise,
   .iova = qnx_bo_iova,
   .set_name = qnx_bo_set_name,
   .dmabuf = qnx_bo_dmabuf,
   .destroy = qnx_bo_destroy,
};

struct fd_bo *
qnx_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags)
{
   struct qnx_device *qdev = to_qnx_device(dev);
   struct qnx_bo *qbo = calloc(1, sizeof(*qbo));

   if (!qbo)
      return NULL;
   if (qfd_bo_new(qdev->transport, size, 0, &qbo->transport) != 0) {
      free(qbo);
      return NULL;
   }
   qbo->base.size = qfd_bo_size(qbo->transport);
   qbo->base.handle = qdev->next_handle++;
   if (!qbo->base.handle)
      qbo->base.handle = qdev->next_handle++;
   qbo->base.map = qfd_bo_map(qbo->transport);
   qbo->base.funcs = &qnx_bo_funcs;
   fd_bo_init_common(&qbo->base, dev);
   return &qbo->base;
}

struct fd_bo *
qnx_bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle)
{
   return NULL;
}

uint32_t
qnx_handle_from_dmabuf(struct fd_device *dev, int fd)
{
   return 0;
}

struct fd_bo *
qnx_bo_from_dmabuf(struct fd_device *dev, int fd)
{
   return NULL;
}

void
qnx_bo_close_handle(struct fd_bo *bo)
{
}
