#include "qnx_priv.h"

static int
qnx_clean_bo(struct fd_bo *bo)
{
   return qfd_bo_cache(to_qnx_bo(bo)->transport, 0, bo->size,
                       QFD_CACHE_OPERATION_1);
}

static int
qnx_flush_submit_list(struct list_head *submit_list)
{
   struct fd_submit *last = last_submit(submit_list);
   struct fd_submit_sp *target = to_fd_submit_sp(last);
   struct qnx_pipe *qpipe = to_qnx_pipe(last->pipe);
   qfd_ib_t ibs[QFD_MAX_SUBMIT_IBS];
   uint32_t nr_ibs = 0;
   uint32_t timestamp = 0;
   int ret;

   foreach_submit_safe (submit, submit_list) {
      struct fd_ringbuffer_sp *primary =
         to_fd_ringbuffer_sp(submit->primary);

      for (unsigned i = 0; i < primary->u.nr_cmds; i++) {
         struct fd_bo *ring_bo = primary->u.cmds[i].ring_bo;

         if (nr_ibs == QFD_MAX_SUBMIT_IBS)
            return -E2BIG;
         fd_submit_append_bo(target, ring_bo);
         ibs[nr_ibs].bo = to_qnx_bo(ring_bo)->transport;
         ibs[nr_ibs].offset_bytes = submit_offset(ring_bo, primary->offset);
         ibs[nr_ibs].size_dwords = primary->u.cmds[i].size / 4;
         nr_ibs++;
      }

      if (submit == last)
         break;

      struct fd_submit_sp *deferred = to_fd_submit_sp(submit);
      for (unsigned i = 0; i < deferred->nr_bos; i++)
         fd_submit_append_bo(target, deferred->bos[i]);
      list_del(&submit->node);
      fd_submit_del(submit);
   }

   for (unsigned i = 0; i < target->nr_bos; i++) {
      ret = qnx_clean_bo(target->bos[i]);
      if (ret)
         return ret;
   }

   ret = qfd_pipe_submit(qpipe->transport, ibs, nr_ibs, 0, NULL,
                         &timestamp);
   if (ret)
      return ret;
   target->out_fence->kfence = timestamp;
   target->out_fence->fence_fd = -1;
   return 0;
}

struct fd_submit *
qnx_submit_sp_new(struct fd_pipe *pipe)
{
   return fd_submit_sp_new(pipe, qnx_flush_submit_list);
}
