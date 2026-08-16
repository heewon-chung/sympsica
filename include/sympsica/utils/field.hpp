#ifndef SYMPSICA_UTILS_FIELD_HPP
#define SYMPSICA_UTILS_FIELD_HPP

#include <limits>

static_assert(std::numeric_limits<unsigned __int128>::digits == 128,
              "build with -std=gnu++20");

#endif // SYMPSICA_UTILS_FIELD_HPP
