#pragma once

#if defined(__QNX__)

// QNX synchronization objects are keyed by their address.  libstdc++'s
// default inline std::mutex uses only PTHREAD_MUTEX_INITIALIZER and has a
// no-op destructor, which leaves a destroyed/reused heap address associated
// with the old kernel object.  Force the gthread wrapper to pair explicit
// pthread_mutex_init()/pthread_mutex_destroy() calls for every lifetime.
#define _GTHREAD_USE_MUTEX_INIT_FUNC

#include <stddef.h>
#include <math.h>

static inline size_t ppsspp_qnx_strnlen(const char *text, size_t maxLength) {
	size_t length = 0;
	while (length < maxLength && text[length] != '\0')
		++length;
	return length;
}

#define strnlen ppsspp_qnx_strnlen

#endif
