/*
 * stddef.h for compiling the decomp with GCC.
 *
 * The decomp's include/stddef.h is empty (its body is #if 0'd, since IDO's
 * ultratypes.h defined these types itself), but because include/ is on the
 * search path it hides the compiler's own stddef.h. This directory comes
 * first on the path. The definitions use GCC's built-in types, so they
 * always match newlib and libctru.
 *
 * Defining _STDDEF_H_ also keeps the decomp's empty copy from being
 * processed afterwards.
 */
#ifndef GE3DS_COMPAT_STDDEF_H
#define GE3DS_COMPAT_STDDEF_H
#define _STDDEF_H_

typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__   wchar_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
