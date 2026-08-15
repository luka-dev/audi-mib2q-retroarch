#pragma once

#if defined(__QNX__)

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
