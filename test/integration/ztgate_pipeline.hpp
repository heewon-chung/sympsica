#ifndef SYMPSICA_TEST_INTEGRATION_ZTGATE_PIPELINE_HPP
#define SYMPSICA_TEST_INTEGRATION_ZTGATE_PIPELINE_HPP

// test/integration/ztgate_pipeline.hpp — thin forwarding shim (task-16-
// brief.md, W5.1, R-HOST consolidation). The W2.1 pipeline this header used
// to declare directly now lives in production, under
// include/sympsica/protocols/detail/ztgate_pipeline.hpp, so that Phase 5's
// Setup (src/protocols/setup.cpp) can call it as an ordinary production
// entry point (R-HOST forbids production code including a test header or
// calling a "test copy"). This shim keeps every existing Phase 2-4 test
// include (`#include "ztgate_pipeline.hpp"` / `"../integration/
// ztgate_pipeline.hpp"`) compiling unchanged, with zero call-site edits: the
// symbols still live in `sympsica::ztgate`, identical to before the move.
#include "sympsica/protocols/detail/ztgate_pipeline.hpp"

#endif // SYMPSICA_TEST_INTEGRATION_ZTGATE_PIPELINE_HPP
