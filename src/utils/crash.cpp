#include "sympsica/utils/crash.hpp"

#include <cstdlib>
#include <cstring>

namespace sympsica {

namespace {

// Read once per process (magic-static init is thread-safe and happens
// exactly once, on the first call from ANY thread) -- see crash.hpp's
// caching note.
const char* crash_target() {
    static const char* target = std::getenv("SYMPSICA_CRASH_AT");
    return target;
}

} // namespace

void crash_point(const char* name) {
    const char* target = crash_target();
    if (target != nullptr && std::strcmp(target, name) == 0) {
        std::_Exit(137);
    }
}

} // namespace sympsica
