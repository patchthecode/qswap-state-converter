#pragma once
// Offline access to the exact serialized Collection layout; never used by the contract.
#define NO_UEFI
#define NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi_types.h"
#define private public
#include "qpi/qpi_containers.h"
#undef private
#include "qpi/qpi.h"
#include "qpi/impl/qpi_collection_impl.h"
#include "kangaroo_twelve.h"
#include "schema.h"

inline void setMem(void* p, unsigned long long n, unsigned char v) { std::memset(p, v, n); }
inline void copyMem(void* p, const void* q, unsigned long long n) { std::memcpy(p, q, n); }
static void* __acquireScratchpad(unsigned long long n, bool zero)
{
    void* p = zero ? std::calloc(1, n) : std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
static void __releaseScratchpad(void* p) { std::free(p); }
static void __markContractStateDirty(unsigned int) { throw std::logic_error("Offline tool cannot mutate node state"); }
namespace QPI {
template <typename T, typename U> inline void copyMemory(T& destination, const U& source)
{
    static_assert(sizeof(T) == sizeof(U));
    std::memcpy(static_cast<void*>(&destination), &source, sizeof(T));
}
}
