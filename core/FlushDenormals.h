#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace reckless::core
{
/**
    While it lives, the floating-point unit flushes denormal numbers to zero - on x86-64 both a denormal result and a
    denormal operand (MXCSR's FTZ and DAZ, 0x8040), on 64-bit ARM a denormal result or operand alike (FPCR's FZ, bit
    24) - and puts the mode back as it found it after. A recursive filter decaying into silence passes through the
    denormal range, where a sample can cost a hundred times a normal one; the series' engines are written to be
    exact with the flush on, and a format's layer raises one round every block it runs.
*/
class ScopedFlushDenormals
{
  public:
    /** Save the floating-point control state and raise the flush bits. */
    ScopedFlushDenormals() noexcept : saved_(Read())
    {
        Write(saved_ | kMask);
    }

    /** Put the floating-point control state back as the constructor found it. */
    ~ScopedFlushDenormals()
    {
        Write(saved_);
    }

    ScopedFlushDenormals(const ScopedFlushDenormals &) = delete; ///< Not copyable: the mode is put back once.
    ScopedFlushDenormals &operator=(const ScopedFlushDenormals &) = delete; ///< Not assignable.

  private:
#if defined(__x86_64__) || defined(_M_X64)
    static constexpr std::uintptr_t kMask = 0x8040; ///< MXCSR's FTZ (bit 15) and DAZ (bit 6).

    /** Read MXCSR.
        @return  the control and status register's bits */
    static std::uintptr_t Read() noexcept
    {
        return static_cast<std::uintptr_t>(_mm_getcsr());
    }

    /** Write MXCSR.
        @param value  the register's new bits */
    static void Write(std::uintptr_t value) noexcept
    {
        _mm_setcsr(static_cast<unsigned int>(value));
    }
#elif defined(__aarch64__)
    static constexpr std::uintptr_t kMask = std::uintptr_t{1} << 24; ///< FPCR's FZ bit.

    /** Read FPCR.
        @return  the floating-point control register's bits */
    static std::uintptr_t Read() noexcept
    {
        std::uintptr_t value = 0;
        asm volatile("mrs %0, fpcr" : "=r"(value));
        return value;
    }

    /** Write FPCR.
        @param value  the register's new bits */
    static void Write(std::uintptr_t value) noexcept
    {
        asm volatile("msr fpcr, %0" : : "r"(value));
    }
#else
    static constexpr std::uintptr_t kMask = 0; ///< No flush bits on this architecture.

    /** No control register on this architecture.
        @return  zero */
    static std::uintptr_t Read() noexcept
    {
        return 0;
    }

    /** No control register on this architecture: does nothing; the argument is ignored. */
    static void Write(std::uintptr_t) noexcept
    {
    }
#endif

    std::uintptr_t saved_; ///< The control state found at construction, put back at destruction.
};
} // namespace reckless::core
