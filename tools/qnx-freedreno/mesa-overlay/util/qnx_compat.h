#ifndef MESA_QNX_COMPAT_H
#define MESA_QNX_COMPAT_H

#if defined(__QNXNTO__) || defined(__QNX__)

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#ifndef __cplusplus
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

int vasprintf(char **result, const char *format, va_list args);
int asprintf(char **result, const char *format, ...);
int mkstemps(char *template_name, int suffix_length);
size_t strnlen(const char *string, size_t max_length);
char *strndup(const char *string, size_t max_length);
ssize_t getline(char **line, size_t *capacity, FILE *stream);

#ifdef __cplusplus
}
#endif

#endif

#endif
