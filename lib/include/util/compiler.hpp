#ifndef EXSIM_UTIL_COMPILER_HPP
#define EXSIM_UTIL_COMPILER_HPP

#if defined(__GNUC__) || defined(__clang__)
#define EXSIM_LIKELY(x)   __builtin_expect(!!(x), 1)
#define EXSIM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define EXSIM_LIKELY(x)   (x)
#define EXSIM_UNLIKELY(x) (x)
#endif

#endif
