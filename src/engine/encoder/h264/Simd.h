// Which vector kernels the H.264 encoder compiles: SSE2 (x86-64), AVX2 (x86, picked at run time) or
// NEON (64-bit ARM). Every kernel has a plain C++ fallback that gives the same numbers: the bitstream
// must not depend on the platform. SCHNEIDI_H264_NO_SIMD forces the fallbacks (to compare them with
// the vector code on one machine).
#pragma once

// No fused multiply-add from separate multiplies and adds: a contracted a * b + c rounds once instead
// of twice, so the floating-point cost decisions (and with them the stream) would differ between
// targets with FMA (AArch64, x86 built with -march=haswell or later) and without. Clang and GCC
// contract by default where the target has FMA. Each source file includes this header first;
// CMake passes -ffp-contract=off as well.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#if !defined(SCHNEIDI_H264_NO_SIMD)
#if defined(__SSE2__) || defined(_M_X64)
#define SCHNEIDI_H264_SSE2 1
#include <emmintrin.h>
#endif
// AVX2 kernels are compiled for that target only and picked at run time (GCC/Clang on x86).
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#define SCHNEIDI_H264_AVX2 1
#include <immintrin.h>
#endif
// AArch64 always has Advanced SIMD; 32-bit ARM is left to the fallbacks (it lacks the across-vector
// adds the kernels use).
#if (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__ARM_NEON) || defined(_M_ARM64))
#define SCHNEIDI_H264_NEON 1
#include <arm_neon.h>
#endif
#endif

namespace schneidi::h264 {

// Busy-wait hint for spin loops (lets the sibling hyper-thread or a slower core run).
inline void cpuRelax()
{
#if defined(SCHNEIDI_H264_SSE2)
    _mm_pause();
#elif defined(SCHNEIDI_H264_NEON) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#endif
}

} // namespace schneidi::h264
