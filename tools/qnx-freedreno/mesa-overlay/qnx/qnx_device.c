#include "qnx_priv.h"

static void
qnx_device_destroy(struct fd_device *dev)
{
   struct qnx_device *qdev = to_qnx_device(dev);

   if (qdev->transport)
      qfd_device_close(qdev->transport);
}

static const struct fd_device_funcs qnx_device_funcs = {
   .bo_new = qnx_bo_new,
   .bo_from_handle = qnx_bo_from_handle,
   .handle_from_dmabuf = qnx_handle_from_dmabuf,
   .bo_from_dmabuf = qnx_bo_from_dmabuf,
   .bo_close_handle = qnx_bo_close_handle,
   .pipe_new = qnx_pipe_new,
   .destroy = qnx_device_destroy,
};

struct fd_device *
qnx_device_new(void)
{
   struct qnx_device *qdev = calloc(1, sizeof(*qdev));

   if (!qdev)
      return NULL;
   if (qfd_device_open(NULL, 0, 0, &qdev->transport) != 0) {
      free(qdev);
      return NULL;
   }

   qfd_device_get_info(qdev->transport, &qdev->info);
   qdev->next_handle = 1;
   qdev->base.funcs = &qnx_device_funcs;
   qdev->base.version = FD_VERSION_SOFTPIN;
   qdev->base.bo_size = sizeof(struct qnx_bo);
   return &qdev->base;
}
