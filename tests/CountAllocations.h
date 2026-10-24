#pragma once

// Counting the allocations a thread makes: every global operator new of the binary replaced by one that counts while
// its thread asks it to, so a count of zero over an audio callback is a callback that allocated nothing - the
// engine's, the core's and the queue's alike - on whichever thread runs it: the test's own (the offline device's Run)
// or an API's audio thread (a counting object made at the top of the callback). The replacements are definitions, not
// declarations, so this header is included by exactly one translation unit of a binary: the audio layer's two suites
// each include it from the file whose callbacks they count.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h> // _aligned_malloc, _aligned_free
#endif

namespace reckless::test
{
namespace allocations
{
inline thread_local bool        t_counting = false; ///< true while this thread's allocations are counted.
inline thread_local std::size_t t_count    = 0;     ///< the allocations this thread made while counting.

/** The replacement operator new's body: counts the call while this thread counts, then mallocs.
    @param size  the bytes asked for; zero is given one byte
    @return      the block, never null (std::bad_alloc is thrown instead) */
inline void *Allocate(std::size_t size)
{
    if (t_counting)
    {
        ++t_count;
    }
    if (void *p = std::malloc(size > 0 ? size : 1))
    {
        return p;
    }
    throw std::bad_alloc();
}

/** The aligned operator new's body: counts the call while this thread counts, then allocates aligned.
    @param size       the bytes asked for; zero is given one byte
    @param alignment  the alignment, in bytes
    @return           the block, never null (std::bad_alloc is thrown instead) */
inline void *AllocateAligned(std::size_t size, std::align_val_t alignment)
{
    if (t_counting)
    {
        ++t_count;
    }
    const auto align = std::max(static_cast<std::size_t>(alignment), sizeof(void *));
#if defined(_MSC_VER)
    if (void *p = _aligned_malloc(size > 0 ? size : 1, align))
    {
        return p;
    }
#else
    void *p = nullptr;
    if (posix_memalign(&p, align, size > 0 ? size : 1) == 0)
    {
        return p;
    }
#endif
    throw std::bad_alloc();
}

/** Frees a block AllocateAligned made.
    @param p  the block, or nullptr */
inline void FreeAligned(void *p) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace allocations

/** Counts the allocations its thread makes while it lives. */
class CountAllocations
{
  public:
    /** Zero this thread's count and start counting its allocations. */
    CountAllocations() noexcept
    {
        allocations::t_count    = 0;
        allocations::t_counting = true;
    }

    /** Stop counting this thread's allocations. */
    ~CountAllocations()
    {
        allocations::t_counting = false;
    }

    CountAllocations(const CountAllocations &)            = delete;
    CountAllocations &operator=(const CountAllocations &) = delete;

    /** The allocations counted since this object was made.
        @return  the count, zero for a thread that allocated nothing */
    std::size_t count() const noexcept
    {
        return allocations::t_count;
    }
};
} // namespace reckless::test

void *operator new(std::size_t size)
{
    return reckless::test::allocations::Allocate(size);
}

void *operator new[](std::size_t size)
{
    return reckless::test::allocations::Allocate(size);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
    return reckless::test::allocations::AllocateAligned(size, alignment);
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return reckless::test::allocations::AllocateAligned(size, alignment);
}

void operator delete(void *p) noexcept
{
    std::free(p);
}

void operator delete[](void *p) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete[](void *p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::align_val_t) noexcept
{
    reckless::test::allocations::FreeAligned(p);
}

void operator delete[](void *p, std::align_val_t) noexcept
{
    reckless::test::allocations::FreeAligned(p);
}

void operator delete(void *p, std::size_t, std::align_val_t) noexcept
{
    reckless::test::allocations::FreeAligned(p);
}

void operator delete[](void *p, std::size_t, std::align_val_t) noexcept
{
    reckless::test::allocations::FreeAligned(p);
}
