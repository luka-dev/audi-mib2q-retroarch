#ifndef FREEDRENO_QNX_PRIV_H
#define FREEDRENO_QNX_PRIV_H

#include "drm/freedreno_priv.h"
#include "drm/freedreno_ringbuffer_sp.h"
#include "qfd_winsys.h"

struct qnx_device {
   struct fd_device base;
   qfd_device_t *transport;
   qfd_device_info_t info;
   uint32_t next_handle;
};
FD_DEFINE_CAST(fd_device, qnx_device);

struct qnx_pipe {
   struct fd_pipe base;
   qfd_pipe_t *transport;
};
FD_DEFINE_CAST(fd_pipe, qnx_pipe);

struct qnx_bo {
   struct fd_bo base;
   qfd_bo_t *transport;
};
FD_DEFINE_CAST(fd_bo, qnx_bo);

struct fd_device *qnx_device_new(void);
struct fd_bo *qnx_bo_new(struct fd_device *dev, uint32_t size,
                         uint32_t flags);
struct fd_bo *qnx_bo_from_handle(struct fd_device *dev, uint32_t size,
                                 uint32_t handle);
uint32_t qnx_handle_from_dmabuf(struct fd_device *dev, int fd);
struct fd_bo *qnx_bo_from_dmabuf(struct fd_device *dev, int fd);
void qnx_bo_close_handle(struct fd_bo *bo);
struct fd_pipe *qnx_pipe_new(struct fd_device *dev, enum fd_pipe_id id,
                             unsigned prio);
struct fd_submit *qnx_submit_sp_new(struct fd_pipe *pipe);

#endif
