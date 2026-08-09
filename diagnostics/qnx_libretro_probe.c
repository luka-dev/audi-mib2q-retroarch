#include <dlfcn.h>
#include <stdio.h>

struct retro_system_info
{
   const char *library_name;
   const char *library_version;
   const char *valid_extensions;
   int need_fullpath;
   int block_extract;
};

int main(int argc, char **argv)
{
   void *core;
   unsigned (*api_version)(void);
   void (*get_system_info)(struct retro_system_info *);
   struct retro_system_info info = {0};

   if (argc != 2)
   {
      fprintf(stderr, "usage: %s CORE.so\n", argv[0]);
      return 2;
   }

   core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!core)
   {
      fprintf(stderr, "dlopen failed: %s\n", dlerror());
      return 3;
   }

   api_version = (unsigned (*)(void))dlsym(core, "retro_api_version");
   get_system_info = (void (*)(struct retro_system_info *))
         dlsym(core, "retro_get_system_info");
   if (!api_version || !get_system_info)
   {
      fprintf(stderr, "required libretro exports missing: %s\n", dlerror());
      dlclose(core);
      return 4;
   }

   get_system_info(&info);
   printf("api=%u name=%s version=%s extensions=%s fullpath=%d extract=%d\n",
         api_version(), info.library_name ? info.library_name : "(null)",
         info.library_version ? info.library_version : "(null)",
         info.valid_extensions ? info.valid_extensions : "(null)",
         info.need_fullpath, info.block_extract);

   dlclose(core);
   return 0;
}
