#ifndef _GNU_SOURCE
#define _GNU_SOURCE // *_np
#endif
#ifdef _3DS
#include <3ds/svc.h>
#include <3ds/os.h>
#include <3ds/services/apt.h>
#include <sys/time.h>
#include "../libpcsxcore/new_dynarec/new_dynarec.h"
static bool is_new_3ds;
#endif
#include "pcsxr-threads.h"

#ifdef __BLACKBERRY_QNX__
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#endif

int pcsxr_sthread_core_count;

// pcsxr "extensions"
extern void SysPrintf(const char *fmt, ...);

#ifndef USE_C11_THREADS

#include "../deps/libretro-common/rthreads/rthreads.c"
#include "features/features_cpu.h"

#define CORE_COUNT() cpu_features_get_core_amount()

static void pcsxr_sthread_lib_init(void)
{
#ifdef _3DS
	int64_t version = 0;
	int fpscr = -1;

	APT_CheckNew3DS(&is_new_3ds);
	svcGetSystemInfo(&version, 0x10000, 0);

	APT_SetAppCpuTimeLimit(35);
	u32 percent = -1;
	APT_GetAppCpuTimeLimit(&percent);

	__asm__ volatile("fmrx %0, fpscr" : "=r"(fpscr));
	SysPrintf("%s3ds detected, v%d.%d, AppCpuTimeLimit=%ld fpscr=%08x\n",
		is_new_3ds ? "new" : "old", (int)GET_VERSION_MAJOR(version),
		(int)GET_VERSION_MINOR(version), percent, fpscr);
#endif
}

#ifdef __BLACKBERRY_QNX__
struct pcsxr_qnx_thread_start
{
	void (*func)(void *);
	enum pcsxr_thread_type type;
	unsigned runmask;
};

static unsigned pcsxr_qnx_thread_runmask(enum pcsxr_thread_type type)
{
	unsigned cores = (unsigned)pcsxr_sthread_core_count;
	unsigned core;

	if (cores <= 1)
		core = 0;
	else switch (type) {
	case PCSXRT_CDR:
	case PCSXRT_SPU:
		core = 1;
		break;
	case PCSXRT_GPU:
		core = cores > 2 ? 2 : 1;
		break;
	case PCSXRT_DRC:
		core = cores > 3 ? 3 : cores - 1;
		break;
	case PCSXRT_COUNT:
	default:
		core = 0;
		break;
	}

	return 1u << core;
}

static void pcsxr_qnx_thread_entry(void *arg)
{
	struct pcsxr_qnx_thread_start *start = arg;
	void (*func)(void *) = start->func;
	enum pcsxr_thread_type type = start->type;
	unsigned requested_runmask = start->runmask;
	unsigned runmask = requested_runmask;

	free(start);
	if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET, &runmask) == -1)
		SysPrintf("QNX pcsxt %d affinity failed: %s\n", type,
			strerror(errno));
	else
		SysPrintf("QNX pcsxt %d affinity mask 0x%x\n", type,
			requested_runmask);

	func(NULL);
}
#endif

sthread_t *pcsxr_sthread_create(void (*thread_func)(void *),
	enum pcsxr_thread_type type)
{
	sthread_t *h = NULL;
#ifdef _3DS
	size_t stack_size = 64*1024;
	Thread ctr_thread;
	int core_id = 0;
	s32 prio = 0x30;

	h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;

	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

	switch (type) {
	case PCSXRT_CDR:
	case PCSXRT_SPU:
		core_id = 1;
		break;
	case PCSXRT_DRC:
		stack_size = new_dynarec_estimate_stack_size();
		// fallthrough
	case PCSXRT_GPU:
		core_id = is_new_3ds ? 2 : 1;
		break;
	case PCSXRT_COUNT:
		break;
	}

	ctr_thread = threadCreate(thread_func, NULL, stack_size, prio, core_id, false);
	if (!ctr_thread) {
		if (core_id == 1) {
			SysPrintf("threadCreate pcsxt %d core %d failed\n",
				type, core_id);
			core_id = is_new_3ds ? 2 : -1;
			ctr_thread = threadCreate(thread_func, NULL, stack_size,
				prio, core_id, false);
		}
	}
	SysPrintf("threadCreate: pcsxt %d core %d stack %zd: %p\n",
		type, core_id, stack_size, ctr_thread);
	if (!ctr_thread) {
		free(h);
		return NULL;
	}
	h->id = (pthread_t)ctr_thread;
#elif defined(__BLACKBERRY_QNX__)
	struct pcsxr_qnx_thread_start *start = calloc(1, sizeof(*start));

	if (!start)
		return NULL;
	start->func = thread_func;
	start->type = type;
	start->runmask = pcsxr_qnx_thread_runmask(type);
	h = sthread_create(pcsxr_qnx_thread_entry, start);
	if (!h)
		free(start);
#else
	h = sthread_create(thread_func, NULL);
 #if defined(__GLIBC__) || \
    (defined(__ANDROID_API__) && __ANDROID_API__ >= 26)
	if (h && (unsigned int)type < (unsigned int)PCSXRT_COUNT)
	{
		const char * const pcsxr_tnames[PCSXRT_COUNT] = {
			"pcsxr-cdrom", "pcsxr-drc", "pcsxr-gpu", "pcsxr-spu"
		};
		pthread_setname_np(h->id, pcsxr_tnames[type]);
	}
 #endif
#endif
	return h;
}

#else // USE_C11_THREADS

#include <unistd.h>
#define CORE_COUNT() sysconf(_SC_NPROCESSORS_ONLN)
#define pcsxr_sthread_lib_init()

#endif

void pcsxr_sthread_init(void)
{
	pcsxr_sthread_core_count = CORE_COUNT();
	if (pcsxr_sthread_core_count < 1)
		pcsxr_sthread_core_count = 1;
	SysPrintf("%d cpu core(s) detected\n", pcsxr_sthread_core_count);

	pcsxr_sthread_lib_init();
}
