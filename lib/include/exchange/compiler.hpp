#ifndef LOB_COMPILER_HPP
#define LOB_COMPILER_HPP

#if defined(__GNUC__) || defined(__clang__)
#define LOB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define LOB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LOB_LIKELY(x)   (x)
#define LOB_UNLIKELY(x) (x)
#endif

#endif
