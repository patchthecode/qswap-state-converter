#pragma once

// Header file for the inclusion of plartform specific x86/x64 intrinsics header files.

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <immintrin.h>
#include <cpuid.h>

#ifdef __cpuid
#undef __cpuid
#endif

static inline void qubicCpuid(int cpuInfo[4], int leaf)
{
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    __cpuid_count(leaf, 0, eax, ebx, ecx, edx);
    cpuInfo[0] = static_cast<int>(eax);
    cpuInfo[1] = static_cast<int>(ebx);
    cpuInfo[2] = static_cast<int>(ecx);
    cpuInfo[3] = static_cast<int>(edx);
}

#define __cpuid(cpuInfo, leaf) qubicCpuid(cpuInfo, leaf)

static inline char _InterlockedCompareExchange8(volatile char* destination, char exchange, char comparand)
{
    char expected = comparand;
    __atomic_compare_exchange_n(destination, &expected, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

static inline char _InterlockedExchange8(volatile char* destination, char value)
{
    return __atomic_exchange_n(destination, value, __ATOMIC_SEQ_CST);
}

static inline long _InterlockedCompareExchange(volatile long* destination, long exchange, long comparand)
{
    return __sync_val_compare_and_swap(destination, comparand, exchange);
}

static inline long _InterlockedExchange(volatile long* destination, long value)
{
    return __atomic_exchange_n(destination, value, __ATOMIC_SEQ_CST);
}

static inline long _InterlockedIncrement(volatile long* destination)
{
    return __atomic_add_fetch(destination, 1, __ATOMIC_SEQ_CST);
}

static inline long _InterlockedDecrement(volatile long* destination)
{
    return __atomic_sub_fetch(destination, 1, __ATOMIC_SEQ_CST);
}

static inline long long _InterlockedCompareExchange64(volatile long long* destination, long long exchange, long long comparand)
{
    return __sync_val_compare_and_swap(destination, comparand, exchange);
}

static inline long long _InterlockedExchange64(volatile long long* destination, long long value)
{
    return __atomic_exchange_n(destination, value, __ATOMIC_SEQ_CST);
}

static inline long long _InterlockedIncrement64(volatile long long* destination)
{
    return __atomic_add_fetch(destination, 1, __ATOMIC_SEQ_CST);
}

static inline long long _InterlockedExchangeAdd64(volatile long long* destination, long long value)
{
    return __atomic_fetch_add(destination, value, __ATOMIC_SEQ_CST);
}

static inline long long _interlockedadd64(volatile long long* destination, long long value)
{
    return __atomic_add_fetch(destination, value, __ATOMIC_SEQ_CST);
}

static inline long long _InterlockedAnd64(volatile long long* destination, long long value)
{
    return __atomic_fetch_and(destination, value, __ATOMIC_SEQ_CST);
}

static inline unsigned long long _umul128(unsigned long long multiplier, unsigned long long multiplicand, unsigned long long* highProduct)
{
    const unsigned __int128 product = static_cast<unsigned __int128>(multiplier) * static_cast<unsigned __int128>(multiplicand);
    *highProduct = static_cast<unsigned long long>(product >> 64);
    return static_cast<unsigned long long>(product);
}

static inline long long _mul128(long long multiplier, long long multiplicand, long long* highProduct)
{
    const __int128 product = static_cast<__int128>(multiplier) * static_cast<__int128>(multiplicand);
    *highProduct = static_cast<long long>(product >> 64);
    return static_cast<long long>(product);
}

static inline unsigned long long __shiftleft128(unsigned long long lowPart, unsigned long long highPart, unsigned char shift)
{
    const unsigned __int128 value = (static_cast<unsigned __int128>(highPart) << 64) | lowPart;
    return static_cast<unsigned long long>((value << shift) >> 64);
}

static inline unsigned long long __shiftright128(unsigned long long lowPart, unsigned long long highPart, unsigned char shift)
{
    const unsigned __int128 value = (static_cast<unsigned __int128>(highPart) << 64) | lowPart;
    return static_cast<unsigned long long>(value >> shift);
}
#endif
