#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "compiler/glsl_types.h"
#include "freedreno_screen.h"

#define PROBE_WIDTH 16
#define PROBE_HEIGHT 16

static int
run_clear_readback(struct pipe_screen *screen, struct pipe_context *context,
                   int require_readback)
{
   struct pipe_resource templ = {0};
   struct pipe_resource *resource = NULL;
   struct pipe_transfer *transfer = NULL;
   struct pipe_fence_handle *fence = NULL;
   struct pipe_framebuffer_state fb = {0};
   union pipe_color_union color = {0};
   const uint8_t expected[4] = {64, 128, 191, 255};
   uint8_t *map;
   uint8_t *pixel;
   int submit_ok = 0;
   int status = 1;

   if (!screen->is_format_supported(screen, PIPE_FORMAT_R8G8B8A8_UNORM,
                                    PIPE_TEXTURE_2D, 1, 1,
                                    PIPE_BIND_RENDER_TARGET)) {
      fprintf(stderr, "qnx_mesa_clear: RGBA8 render target unsupported\n");
      return 1;
   }

   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = PROBE_WIDTH;
   templ.height0 = PROBE_HEIGHT;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_RENDER_TARGET;

   resource = screen->resource_create(screen, &templ);
   if (!resource) {
      fprintf(stderr, "qnx_mesa_clear: resource_create failed\n");
      goto done;
   }

   fb.width = PROBE_WIDTH;
   fb.height = PROBE_HEIGHT;
   fb.nr_cbufs = 1;
   fb.cbufs[0].texture = resource;
   fb.cbufs[0].format = templ.format;
   context->set_framebuffer_state(context, &fb);

   color.f[0] = 0.25f;
   color.f[1] = 0.50f;
   color.f[2] = 0.75f;
   color.f[3] = 1.00f;
   context->clear(context, PIPE_CLEAR_COLOR0, 0xf, 0, NULL, &color, 0.0, 0);
   context->flush(context, &fence, 0);
   if (!fence || !screen->fence_finish(screen, context, fence, 5000000000ULL)) {
      fprintf(stderr, "qnx_mesa_clear: fence wait failed\n");
      goto done;
   }
   submit_ok = 1;
   puts("qnx_mesa_clear_submit: PASS");

   map = pipe_texture_map(context, resource, 0, 0, PIPE_MAP_READ,
                          0, 0, PROBE_WIDTH, PROBE_HEIGHT, &transfer);
   if (!map) {
      fprintf(stderr, "qnx_mesa_clear: texture_map failed\n");
      goto done;
   }

   pixel = map + (PROBE_HEIGHT / 2) * transfer->stride +
           (PROBE_WIDTH / 2) * 4;
   printf("qnx_mesa_clear_pixel=%u,%u,%u,%u stride=%u\n",
          pixel[0], pixel[1], pixel[2], pixel[3], transfer->stride);
   status = 0;
   for (unsigned i = 0; i < 4; i++) {
      int delta = (int)pixel[i] - (int)expected[i];
      if (delta < -1 || delta > 1)
         status = 1;
   }

done:
   if (transfer)
      pipe_texture_unmap(context, transfer);
   if (fence)
      screen->fence_reference(screen, &fence, NULL);
   if (resource) {
      struct pipe_framebuffer_state empty = {0};
      context->set_framebuffer_state(context, &empty);
      pipe_resource_reference(&resource, NULL);
   }

   if (status == 0) {
      puts("qnx_mesa_clear_readback: PASS");
   } else if (!require_readback && submit_ok) {
      puts("qnx_mesa_clear_readback: SKIP "
           "(QEMU render targets remain in the host FBO)");
      status = 0;
   } else {
      fprintf(stderr, "qnx_mesa_clear_readback: FAIL\n");
   }
   return status;
}

static int
usage(const char *program)
{
   fprintf(stderr, "usage: %s {--qemu-only|--hardware}\n", program);
   return 2;
}

int
main(int argc, char **argv)
{
   struct pipe_screen *screen;
   struct pipe_context *context;
   const char *name;
   const char *vendor;
   int require_readback;

   if (argc != 2 ||
       (strcmp(argv[1], "--qemu-only") != 0 &&
        strcmp(argv[1], "--hardware") != 0))
      return usage(argv[0]);
   require_readback = strcmp(argv[1], "--hardware") == 0;

   glsl_type_singleton_init_or_ref();
   screen = fd_screen_create(-1, NULL, NULL);
   if (!screen) {
      fprintf(stderr, "qnx_screen_probe: fd_screen_create failed\n");
      glsl_type_singleton_decref();
      return 1;
   }

   name = screen->get_name(screen);
   vendor = screen->get_vendor(screen);
   printf("qnx_screen_name=%s\n", name ? name : "(null)");
   printf("qnx_screen_vendor=%s\n", vendor ? vendor : "(null)");

   context = screen->context_create(screen, NULL, 0);
   if (!context) {
      fprintf(stderr, "qnx_screen_probe: context_create failed\n");
      screen->destroy(screen);
      glsl_type_singleton_decref();
      return 1;
   }

   if (run_clear_readback(screen, context, require_readback) != 0) {
      context->destroy(context);
      screen->destroy(screen);
      glsl_type_singleton_decref();
      return 1;
   }

   context->destroy(context);
   screen->destroy(screen);
   glsl_type_singleton_decref();
   puts("qnx_screen_probe: PASS");
   return 0;
}
