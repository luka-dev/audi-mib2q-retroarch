#include "qnx_priv.h"

static int
qnx_pipe_get_param(struct fd_pipe *pipe, enum fd_param_id param,
                   uint64_t *value)
{
   struct qnx_device *qdev = to_qnx_device(pipe->dev);
   uint32_t timestamp;
   int ret;

   switch (param) {
   case FD_DEVICE_ID:
      *value = 1;
      return 0;
   case FD_GPU_ID:
      *value = qdev->info.words[6];
      return 0;
   case FD_GMEM_SIZE:
      *value = qdev->info.words[7];
      return 0;
   case FD_GMEM_BASE:
      *value = qdev->info.words[4];
      return 0;
   case FD_CHIP_ID:
      *value = qdev->info.words[1];
      return 0;
   case FD_TIMESTAMP:
      ret = qfd_pipe_read_timestamp(to_qnx_pipe(pipe)->transport, 2,
                                    &timestamp);
      if (!ret)
         *value = timestamp;
      return ret;
   case FD_NR_PRIORITIES:
      *value = 1;
      return 0;
   case FD_VA_SIZE:
      *value = 32;
      return 0;
   case FD_MAX_FREQ:
   case FD_CTX_FAULTS:
   case FD_GLOBAL_FAULTS:
   case FD_SUSPEND_COUNT:
   case FD_UCHE_TRAP_BASE:
      *value = 0;
      return 0;
   default:
      return -EINVAL;
   }
}

static int
qnx_pipe_set_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t value)
{
   return -ENOSYS;
}

static int
qnx_pipe_wait(struct fd_pipe *pipe, const struct fd_fence *fence,
              uint64_t timeout_ns)
{
   uint32_t timeout_ms;
   int ret;

   if (timeout_ns == UINT64_MAX)
      timeout_ms = UINT32_MAX;
   else
      timeout_ms = MIN2(DIV_ROUND_UP(timeout_ns, 1000000ull), UINT32_MAX);
   ret = qfd_pipe_wait_timestamp(to_qnx_pipe(pipe)->transport,
                                 fence->kfence, timeout_ms);
   if (!ret)
      pipe->control->fence = fence->ufence;
   return ret;
}

static void
qnx_pipe_destroy(struct fd_pipe *pipe)
{
   struct qnx_pipe *qpipe = to_qnx_pipe(pipe);

   fd_pipe_sp_ringpool_fini(pipe);
   qfd_pipe_del(qpipe->transport);
   free(qpipe);
}

static const struct fd_pipe_funcs qnx_pipe_funcs = {
   .ringbuffer_new_object = fd_ringbuffer_sp_new_object,
   .submit_new = qnx_submit_sp_new,
   .flush = fd_pipe_sp_flush,
   .get_param = qnx_pipe_get_param,
   .set_param = qnx_pipe_set_param,
   .wait = qnx_pipe_wait,
   .destroy = qnx_pipe_destroy,
};

struct fd_pipe *
qnx_pipe_new(struct fd_device *dev, enum fd_pipe_id id, unsigned prio)
{
   struct qnx_pipe *qpipe;

   if (id != FD_PIPE_3D)
      return NULL;
   /* GSL exposes one 3D queue.  Freedreno uses priority 1 for its bootstrap
    * pipe and priority 0 when Gallium creates a normal context; both map to
    * that same queue here. */
   (void)prio;
   qpipe = calloc(1, sizeof(*qpipe));
   if (!qpipe)
      return NULL;
   qpipe->base.dev = dev;
   qpipe->base.funcs = &qnx_pipe_funcs;
   if (qfd_pipe_new(to_qnx_device(dev)->transport, QFD_CONTEXT_TYPE_3D, 0,
                    &qpipe->transport) != 0) {
      free(qpipe);
      return NULL;
   }
   fd_pipe_sp_ringpool_init(&qpipe->base);
   return &qpipe->base;
}
