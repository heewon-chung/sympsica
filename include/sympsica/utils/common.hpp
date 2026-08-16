#ifndef SYMPSICA_UTILS_COMMON_HPP
#define SYMPSICA_UTILS_COMMON_HPP

#include <cstdio>
#include <cstdlib>

// SYMPSICA_REQUIRE(cond, msg) — sympsica's single error-handling primitive
// (task-2 brief, "Error policy"): on failure, print `msg` to stderr and
// abort(). PoC-level policy: no exceptions, no recovery paths, used in place
// of assert()/exceptions throughout the codebase.
#define SYMPSICA_REQUIRE(cond, msg)                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "SYMPSICA_REQUIRE failed: %s (%s:%d)\n",    \
                          (msg), __FILE__, __LINE__);                        \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

#endif // SYMPSICA_UTILS_COMMON_HPP
