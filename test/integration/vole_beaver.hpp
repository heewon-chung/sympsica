#ifndef SYMPSICA_TEST_INTEGRATION_VOLE_BEAVER_HPP
#define SYMPSICA_TEST_INTEGRATION_VOLE_BEAVER_HPP

// test/integration/vole_beaver.hpp — thin forwarding shim (task-16-
// brief.md, W5.1, R-HOST consolidation). The W2.2 NoisyVole/Beaver-triple
// assembly this header used to declare directly now lives in production,
// under include/sympsica/protocols/detail/vole_beaver.hpp, so that Phase
// 5's Setup (src/protocols/setup.cpp) can call it as an ordinary production
// entry point (R-HOST forbids production code including a test header or
// calling a "test copy"). This shim keeps every existing Phase 2-4 test
// include (`#include "vole_beaver.hpp"` / `"../integration/vole_beaver.hpp"`)
// compiling unchanged, with zero call-site edits: the symbols still live in
// `sympsica::vole`, identical to before the move.
#include "sympsica/protocols/detail/vole_beaver.hpp"

#endif // SYMPSICA_TEST_INTEGRATION_VOLE_BEAVER_HPP
