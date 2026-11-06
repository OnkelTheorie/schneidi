/* Minimal stand-in for Opus' os_support.h, which the RNNoise sources include from vec.h (plain C kernels) and
   vec_neon.h (ARM) but which was not vendored: without it every non-x86 build failed. Only what those headers use. */
#ifndef OS_SUPPORT_H
#define OS_SUPPORT_H

#include <string.h>

#ifndef OPUS_INLINE
#define OPUS_INLINE inline
#endif

/* Copy n elements from src to dst (no overlap) */
#define OPUS_COPY(dst, src, n) (memcpy((dst), (src), (n) * sizeof(*(dst)) + 0 * ((dst) - (src))))
/* Copy n elements from src to dst (overlap allowed) */
#define OPUS_MOVE(dst, src, n) (memmove((dst), (src), (n) * sizeof(*(dst)) + 0 * ((dst) - (src))))
/* Set n elements of dst to zero */
#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n) * sizeof(*(dst))))

#endif
