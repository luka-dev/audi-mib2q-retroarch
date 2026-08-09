#ifndef MUPEN64PLUS_NEXT_QNX_WCHAR_WRAPPER_H
#define MUPEN64PLUS_NEXT_QNX_WCHAR_WRAPPER_H

/*
 * GCC's private stddef.h defines an empty __WCHAR_T guard.  QNX 6.5's
 * wchar.h mistakes that guard for a concrete type and emits `typedef
 * wchar_t;` in C++ mode.  Consume the guard immediately before entering
 * the SDK header; C++ already has a built-in wchar_t type.
 */
#ifdef __cplusplus
#ifdef __WCHAR_T
#undef __WCHAR_T
#endif
#endif

#include_next <wchar.h>

#endif
