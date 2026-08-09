/* ra_math.c -- override BROKEN interposed math on MHI2Q.
 *
 * The bundled GCC 4.9.4 libstdc++.so.6 exports four-byte float-math stubs such
 * as ceilf/floorf/expf/powf. Each stub branches through its own PLT relocation.
 * With libstdc++ before libm in the process-wide symbol scope, that relocation
 * resolves back to the same libstdc++ stub and spins forever. This was proven
 * on the unit: the stalled PC for ceilf was libstdc++ base + 0x69e00, while the
 * same device libm function fetched with dlopen()+dlsym returned normally.
 *
 * Fix: DEFINE affected symbols in the executable itself. Executable symbols win
 * runtime lookup for RA's calls, and each forwards to the real device libm
 * implementation resolved once via dlsym. Compiled into the griffin unity TU
 * (included from griffin.c). */

#include <dlfcn.h>

static float  (*g_expf)(float);
static double (*g_exp)(double);
static float  (*g_powf)(float, float);
static double (*g_pow)(double, double);
static float  (*g_ceilf)(float);
static float  (*g_floorf)(float);

static void ra_math_bind(void)
{
   /* libm is already loaded (DT_NEEDED); dlopen just hands back its handle. */
   void *h = dlopen("libm.so.2", RTLD_NOW | RTLD_GLOBAL);
   if (!h)
      return;
   g_expf = (float  (*)(float))        dlsym(h, "expf");
   g_exp  = (double (*)(double))       dlsym(h, "exp");
   g_powf = (float  (*)(float, float)) dlsym(h, "powf");
   g_pow  = (double (*)(double, double))dlsym(h, "pow");
   g_ceilf  = (float (*)(float)) dlsym(h, "ceilf");
   g_floorf = (float (*)(float)) dlsym(h, "floorf");
}

float expf(float x)
{
   if (!g_expf) ra_math_bind();
   return g_expf ? g_expf(x) : x;
}

double exp(double x)
{
   if (!g_exp) ra_math_bind();
   return g_exp ? g_exp(x) : x;
}

float powf(float x, float y)
{
   if (!g_powf) ra_math_bind();
   return g_powf ? g_powf(x, y) : x;
}

double pow(double x, double y)
{
   if (!g_pow) ra_math_bind();
   return g_pow ? g_pow(x, y) : x;
}

float ceilf(float x)
{
   if (!g_ceilf) ra_math_bind();
   return g_ceilf ? g_ceilf(x) : x;
}

float floorf(float x)
{
   if (!g_floorf) ra_math_bind();
   return g_floorf ? g_floorf(x) : x;
}
