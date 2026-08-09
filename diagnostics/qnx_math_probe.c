#define __EXT_UNIX_MISC 1
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned read_fpscr(void)
{
   unsigned value;
   /* QNX SDP 6.5 ships an older GNU assembler that uses the pre-UAL
    * mnemonic for VMRS. */
   __asm__ volatile ("fmrx %0, fpscr" : "=r" (value));
   return value;
}

static void report_default_symbol(const char *name)
{
   void *address = dlsym(RTLD_DEFAULT, name);
   Dl_info info;

   memset(&info, 0, sizeof(info));
   if (address && dladdr(address, &info))
      fprintf(stderr,
            "default %s=%p provider=%s base=%p symbol=%s symbol_addr=%p\n",
            name,
            address,
            info.dli_fname ? info.dli_fname : "?",
            info.dli_fbase,
            info.dli_sname ? info.dli_sname : "?",
            info.dli_saddr);
   else
      fprintf(stderr, "default %s=%p provider=?\n", name, address);
   fflush(stderr);
}

int main(int argc, char **argv)
{
   volatile float xf = 91.0f;
   volatile double xd = 91.0;

   if (argc != 2)
      return 2;

   fprintf(stderr, "probe mode=%s fpscr_before=0x%08x\n",
         argv[1], read_fpscr());
   fflush(stderr);

   report_default_symbol(strcmp(argv[1], "dlsym_ceilf") == 0
         ? "ceilf" : argv[1]);

   if (strcmp(argv[1], "floor") == 0)
      fprintf(stderr, "result=%f\n", floor(xd));
   else if (strcmp(argv[1], "floorf") == 0)
      fprintf(stderr, "result=%f\n", (double)floorf(xf));
   else if (strcmp(argv[1], "ceil") == 0)
      fprintf(stderr, "result=%f\n", ceil(xd));
   else if (strcmp(argv[1], "ceilf") == 0)
      fprintf(stderr, "result=%f\n", (double)ceilf(xf));
   else if (strcmp(argv[1], "dlsym_ceilf") == 0)
   {
      void *libm = dlopen("libm.so.2", RTLD_NOW);
      float (*fn)(float);

      fprintf(stderr, "dlopen=%p\n", libm);
      fflush(stderr);
      if (!libm)
         return 3;

      fn = (float (*)(float))dlsym(libm, "ceilf");
      fprintf(stderr, "dlsym=%p\n", fn);
      fflush(stderr);
      if (!fn)
         return 4;

      fprintf(stderr, "result=%f\n", (double)fn(xf));
   }
   else
      return 5;

   fprintf(stderr, "probe done fpscr_after=0x%08x\n", read_fpscr());
   fflush(stderr);
   return 0;
}
