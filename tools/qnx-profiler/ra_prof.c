/* ra_prof - sampling profiler for QNX 6.5 (MHI2Q / APQ8064).
 *
 * WHY NOT tracelogger: the unit runs the instrumented kernel, but a full
 * kernel trace spikes system load enough for the HU watchdog to reset the
 * box (observed 2026-08-22). This samples instead: one devctl per thread per
 * tick, so at the default 100 Hz over ~10 threads it is ~1000 devctls/s -
 * far below anything the watchdog notices.
 *
 * Emits one line per sample:
 *     <ms> <tid> <state> <ip> <blocked_ms>
 * Aggregate it off-target (address -> symbol needs the UNSTRIPPED binary and
 * the load bases, which are written to the header as MAP lines).
 *
 *   ra_prof <pid> <seconds> [hz] > samples.txt
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <devctl.h>
#include <sys/procfs.h>
#include <sys/debug.h>
#include <sys/neutrino.h>
#include <sys/mman.h>   /* MAP_ELF */
#include <limits.h>    /* PATH_MAX */

#define MAX_TIDS 256
/* Thread ids are dense in practice; give up after this many consecutive holes
 * rather than paying 256 devctls per tick to rediscover the same empty tail. */
#define MAX_TID_GAP 24

static uint64_t now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

/* Load bases: an ip is only meaningful once you know which object it fell in
 * and where that object was mapped. */
static void dump_maps(int fd)
{
   procfs_mapinfo *maps;
   int             num = 0, i;

   if (devctl(fd, DCMD_PROC_MAPINFO, NULL, 0, &num) != EOK || num <= 0)
      return;
   if (!(maps = calloc((size_t)num, sizeof(*maps))))
      return;
   if (devctl(fd, DCMD_PROC_MAPINFO, maps,
              (size_t)num * sizeof(*maps), &num) == EOK)
   {
      for (i = 0; i < num; i++)
      {
         /* Names make the profile readable: without them every address
          * outside the frontend collapses onto its last symbol. MAPDEBUG
          * resolves a mapping base to the file it came from. */
         union {
            procfs_debuginfo info;
            char             buf[sizeof(procfs_debuginfo) + PATH_MAX];
         } d;

         memset(&d, 0, sizeof(d));
         d.info.vaddr = maps[i].vaddr;
         if (devctl(fd, DCMD_PROC_MAPDEBUG, &d, sizeof(d), 0) != EOK)
            d.info.path[0] = '\0';

         printf("MAP %llx %llx %llx %x %s\n",
                (unsigned long long)maps[i].vaddr,
                (unsigned long long)maps[i].size,
                (unsigned long long)maps[i].offset,
                (unsigned)maps[i].flags,
                d.info.path[0] ? d.info.path : "-");
      }
   }
   free(maps);
}

int main(int argc, char **argv)
{
   char            path[64];
   int             fd, hz, secs, only;
   uint64_t        t_end, t0;
   pid_t           pid;

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s <pid> <seconds> [hz] [tid]\n"
                      "  tid: sample only that thread, which is what makes a\n"
                      "       short stall visible - one devctl per tick instead\n"
                      "       of one per thread lifts the real rate ~10x.\n",
              argv[0]);
      return 2;
   }
   pid  = (pid_t)strtol(argv[1], NULL, 10);
   secs = (int)strtol(argv[2], NULL, 10);
   hz   = argc > 3 ? (int)strtol(argv[3], NULL, 10) : 100;
   only = argc > 4 ? (int)strtol(argv[4], NULL, 10) : 0;
   if (hz < 1)   hz = 1;
   if (hz > 500) hz = 500;      /* keep well clear of the watchdog */

   snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
   if ((fd = open(path, O_RDONLY)) == -1)
   {
      fprintf(stderr, "open %s: %s\n", path, strerror(errno));
      return 1;
   }

   /* Line-buffered output through an ssh pipe throttled sampling to ~15 Hz no
    * matter what -hz asked for, which silently turned every rate below into a
    * lie.  Buffer the whole run instead; the report only needs it at the end. */
   setvbuf(stdout, NULL, _IOFBF, 1 << 20);

   printf("# ra_prof pid=%d hz=%d secs=%d\n", (int)pid, hz, secs);
   dump_maps(fd);
   printf("# ms tid state ip blocked_ms\n");

   t0    = now_ms();
   t_end = t0 + (uint64_t)secs * 1000u;

   while (now_ms() < t_end)
   {
      uint64_t stamp = now_ms() - t0;
      int      tid;

      int misses = 0;
      int first  = only ? only : 1;
      int last   = only ? only : MAX_TIDS;

      for (tid = first; tid <= last && misses < MAX_TID_GAP; tid++)
      {
         procfs_status st;

         memset(&st, 0, sizeof(st));
         st.tid = (pthread_t)tid;
         if (devctl(fd, DCMD_PROC_TIDSTATUS, &st, sizeof(st), 0) != EOK
               || st.tid != (pthread_t)tid)
         {
            misses++;                     /* tid does not exist (yet) */
            continue;
         }
         misses = 0;

         printf("%llu %d %u %llx %llu\n",
                (unsigned long long)stamp, tid, (unsigned)st.state,
                (unsigned long long)st.ip,
                (unsigned long long)(st.nsec_since_block / 1000000u));
      }
      delay(1000 / hz);
   }

   close(fd);
   return 0;
}
