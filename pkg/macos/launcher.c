#define _DARWIN_C_SOURCE

#include <mach-o/dyld.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
   uint32_t executable_size = PATH_MAX;
   char executable[PATH_MAX];
   char executable_copy[PATH_MAX];
   char project_root[PATH_MAX];
   char retroarch[PATH_MAX];
   char config[PATH_MAX + 16];
   char log_file[PATH_MAX + 16];
   char rules[PATH_MAX];
   char **child_argv;
   char *parent;
   int i;

   if (_NSGetExecutablePath(executable, &executable_size) != 0)
   {
      fputs("RetroArchTest: executable path is too long\n", stderr);
      return 1;
   }

   if (!realpath(executable, executable_copy))
   {
      perror("RetroArchTest: realpath");
      return 1;
   }

   parent = dirname(executable_copy);
   if (snprintf(retroarch, sizeof(retroarch), "%s/retroarch", parent)
         >= (int)sizeof(retroarch))
      return 1;

   if (strlcpy(project_root, parent, sizeof(project_root))
         >= sizeof(project_root))
      return 1;
   for (i = 0; i < 5; i++)
   {
      parent = dirname(project_root);
      if (parent != project_root)
         memmove(project_root, parent, strlen(parent) + 1);
   }

   if (chdir(project_root) != 0)
   {
      perror("RetroArchTest: chdir");
      return 1;
   }

   snprintf(rules, sizeof(rules), "%s/macos-content-rules.cfg", project_root);
   snprintf(config, sizeof(config), "--config=%s/macos-test.cfg", project_root);
   snprintf(log_file, sizeof(log_file),
         "--log-file=%s/out/macos-test/logs/retroarch.log", project_root);
   if (setenv("RA_CONTENT_RULES", rules, 1) != 0)
   {
      perror("RetroArchTest: setenv");
      return 1;
   }

   child_argv = calloc((size_t)argc + 4, sizeof(*child_argv));
   if (!child_argv)
      return 1;
   child_argv[0] = retroarch;
   child_argv[1] = config;
   child_argv[2] = log_file;
   child_argv[3] = "--verbose";
   for (i = 1; i < argc; i++)
      child_argv[i + 3] = argv[i];

   execv(retroarch, child_argv);
   perror("RetroArchTest: execv");
   return 1;
}
