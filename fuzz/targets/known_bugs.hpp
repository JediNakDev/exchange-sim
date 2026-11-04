#ifndef EXSIM_FUZZ_TARGETS_KNOWN_BUGS_HPP
#define EXSIM_FUZZ_TARGETS_KNOWN_BUGS_HPP

// Bugs found by fuzzing in code the fuzzing work does not own (src/, include/).
// They are reported to the owners instead of being fixed here.
//
// While a bug is open the targets steer around its trigger, so a campaign keeps
// exploring instead of stopping at the same crash forever, and `make test`
// stays green. The minimized reproducer lives in
// fuzz/corpus/known_bugs/<target>/ and is replayed only when
// EXSIM_FUZZ_ENFORCE_KNOWN_BUGS is defined:
//
//   make test EXTRA_CXXFLAGS=-DEXSIM_FUZZ_ENFORCE_KNOWN_BUGS FILTER=fuzz_known_bugs
//
// Once an owner fixes a bug: delete its steering predicate, move its
// reproducer into fuzz/corpus/<target>/ so the normal replay keeps it, and
// remove it from docs/FUZZING.md "Open findings".

namespace exsim::fuzz {

#ifdef EXSIM_FUZZ_ENFORCE_KNOWN_BUGS
inline constexpr bool kSteerAroundKnownBugs = false;
#else
inline constexpr bool kSteerAroundKnownBugs = true;
#endif

}  // namespace exsim::fuzz

#endif
