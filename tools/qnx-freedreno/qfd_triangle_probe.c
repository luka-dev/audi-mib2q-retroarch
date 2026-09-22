#include "qfd_winsys.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define QFD_REG_GRAS_CL_VPORT_XOFFSET 0x2048u
#define QFD_REG_GRAS_SC_WINDOW_SCISSOR_BR 0x207au
#define QFD_REG_RB_MRT_CONTROL_0 0x20c4u
#define QFD_CP_LOAD_STATE 0x30u
#define QFD_CP_DRAW_INDX_2 0x36u

#define QFD_TYPE0_HEADER(reg, payload_dwords)                                 \
   ((((payload_dwords) - 1u) << 16) | (reg))
#define QFD_TYPE3_HEADER(opcode, payload_dwords)                              \
   (0xc0000000u | (((payload_dwords) - 1u) << 16) | ((opcode) << 8))

/* Inline SS_DIRECT shader-load dword for one A3xx instruction group. */
static uint32_t qfd_shader_load_word(uint32_t block)
{
   return (1u << 22) | (block << 19);
}

int main(int argc, char **argv)
{
   static const uint32_t vs_ir3[] = {0x00000000u, 0x03000000u};
   static const uint32_t fs_ir3[] = {
      0x00000020u, 0x20244000u,
      0x0c210001u, 0x40104002u,
      0x00000003u, 0x8010000au,
      0x00000000u, 0x03000000u
   };
   qfd_device_t *device = NULL;
   qfd_pipe_t *pipe = NULL;
   qfd_bo_t *ib_bo = NULL;
   qfd_ib_t ib;
   uint32_t *commands;
   uint32_t n = 0;
   uint32_t i;
   uint32_t timestamp = 0;
   uint32_t retired = 0;
   int status;
   int failed = 0;

   if (argc != 2 || strcmp(argv[1], "--qemu-only") != 0)
   {
      fputs("qfd_triangle_probe: REFUSED (requires --qemu-only)\n", stderr);
      return 2;
   }

   puts("qfd_triangle_probe_mode=qemu_only_synthetic_ir3_triangle");
   status = qfd_device_open(NULL, 0, 0, &device);
   printf("qfd_triangle_device_open_status=%d\n", status);
   if (status != 0)
      return 1;

   status = qfd_bo_new(device, 4096u, 0, &ib_bo);
   printf("qfd_triangle_ib_alloc_status=%d\n", status);
   if (status != 0)
   {
      failed = 1;
      goto close_device;
   }
   commands = (uint32_t *)qfd_bo_map(ib_bo);
   if (commands == NULL)
   {
      puts("qfd_triangle_map_status=-1");
      failed = 1;
      goto free_ib;
   }

   commands[n++] = QFD_TYPE0_HEADER(QFD_REG_GRAS_CL_VPORT_XOFFSET, 4u);
   commands[n++] = 0x43800000u;
   commands[n++] = 0x43800000u;
   commands[n++] = 0x43000000u;
   commands[n++] = 0x43000000u;
   commands[n++] = QFD_TYPE0_HEADER(QFD_REG_RB_MRT_CONTROL_0, 1u);
   commands[n++] = 0x0f001c08u;
   commands[n++] =
      QFD_TYPE0_HEADER(QFD_REG_GRAS_SC_WINDOW_SCISSOR_BR, 1u);
   commands[n++] = 0x01df03ffu;

   commands[n++] = QFD_TYPE3_HEADER(QFD_CP_LOAD_STATE, 4u);
   commands[n++] = qfd_shader_load_word(4u);
   commands[n++] = 0;
   for (i = 0; i < (uint32_t)(sizeof(vs_ir3) / sizeof(vs_ir3[0])); ++i)
      commands[n++] = vs_ir3[i];

   commands[n++] = QFD_TYPE3_HEADER(QFD_CP_LOAD_STATE, 10u);
   commands[n++] = qfd_shader_load_word(6u);
   commands[n++] = 0;
   for (i = 0; i < (uint32_t)(sizeof(fs_ir3) / sizeof(fs_ir3[0])); ++i)
      commands[n++] = fs_ir3[i];

   commands[n++] = QFD_TYPE3_HEADER(QFD_CP_DRAW_INDX_2, 5u);
   commands[n++] = 0;
   commands[n++] = 0x44u;
   commands[n++] = 3u;
   commands[n++] = 0x00010000u;
   commands[n++] = 2u;

   printf("qfd_triangle_ib_dwords=%u gpuaddr=0x%08x\n", n,
          qfd_bo_gpuaddr(ib_bo));
   status = qfd_bo_cache(ib_bo, 0, n * 4u, QFD_CACHE_OPERATION_1);
   printf("qfd_triangle_ib_clean_status=%d\n", status);
   if (status != 0)
   {
      failed = 1;
      goto free_ib;
   }

   status = qfd_pipe_new(device, QFD_CONTEXT_TYPE_3D, 0, &pipe);
   printf("qfd_triangle_context_status=%d handle=%u\n", status,
          qfd_pipe_handle(pipe));
   if (status != 0)
   {
      failed = 1;
      goto free_ib;
   }

   ib.bo = ib_bo;
   ib.offset_bytes = 0;
   ib.size_dwords = n;
   status = qfd_pipe_submit(pipe, &ib, 1u, 0, NULL, &timestamp);
   printf("qfd_triangle_issue_status=%d timestamp=%u\n", status, timestamp);
   if (status != 0)
   {
      failed = 1;
      goto destroy_pipe;
   }
   status = qfd_pipe_wait_timestamp(pipe, timestamp, 5000u);
   printf("qfd_triangle_wait_status=%d\n", status);
   failed |= status != 0;
   status = qfd_pipe_read_timestamp(pipe, 2u, &retired);
   printf("qfd_triangle_retired_status=%d value=%u\n", status, retired);
   failed |= status != 0 || !qfd_timestamp_after(retired, timestamp - 1u);

destroy_pipe:
   status = qfd_pipe_del(pipe);
   pipe = NULL;
   printf("qfd_triangle_context_destroy_status=%d\n", status);
   failed |= status != 0;
free_ib:
   status = qfd_bo_del(ib_bo);
   ib_bo = NULL;
   printf("qfd_triangle_ib_free_status=%d\n", status);
   failed |= status != 0;
close_device:
   if (pipe != NULL)
      failed |= qfd_pipe_del(pipe) != 0;
   if (ib_bo != NULL)
      failed |= qfd_bo_del(ib_bo) != 0;
   status = qfd_device_close(device);
   printf("qfd_triangle_device_close_status=%d\n", status);
   failed |= status != 0;
   printf("qfd_triangle_probe: %s\n", failed ? "FAIL" : "PASS");
   return failed ? 1 : 0;
}
