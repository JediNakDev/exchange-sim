#ifndef EXSIM_EXCHANGE_COMPILER_HPP
#define EXSIM_EXCHANGE_COMPILER_HPP

#if defined(__GNUC__) || defined(__clang__)
#define EXSIM_EXCHANGE_LIKELY(x)   __builtin_expect(!!(x), 1)
#define EXSIM_EXCHANGE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define EXSIM_EXCHANGE_LIKELY(x)   (x)
#define EXSIM_EXCHANGE_UNLIKELY(x) (x)
#endif

#endif
