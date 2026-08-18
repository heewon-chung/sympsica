#ifndef SYMPSICA_TEST_GATES_ZTGATE_CONVERT_HPP
#define SYMPSICA_TEST_GATES_ZTGATE_CONVERT_HPP

// test/gates/ztgate_convert.hpp — task-14-brief.md requirement 1(d): the
// test-side converter from the Phase-2 pipeline's output type
// (ztgate::ZtGateOut, test/integration/ztgate_pipeline.hpp) to production's
// core/pools.hpp ZtGate. Production code (gates/ztest.hpp, gates/
// symdiff.hpp) consumes ONLY core::ZtGate; the pipeline's richer ZtGateOut
// (which also exposes mask_half/digit_shares — private test-only
// visibility into a value Setup keeps secret) stays test-only. This keeps
// the "NEVER test-side types in production" rule (task-14-brief.md) intact
// while letting tests generate real gates through the same pipeline
// task-5/task-8 already established.

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "../integration/ztgate_pipeline.hpp"

namespace sympsica_test {

// Field-for-field mapping: corr_id carries straight through, mask_m is the
// pipeline's step-3 additive F_p share of the mask (already the exact
// value Share wraps), key is the pipeline's step-5 multi-point DPF key
// (already the exact type ZtGate::key wants — see task-5-report.md API
// delta 1: RegularDpfKey is not a template).
inline sympsica::ZtGate to_ztgate(const sympsica::ztgate::ZtGateOut& out) {
    return sympsica::ZtGate{out.corr_id, sympsica::Share{out.mask_share}, out.key};
}

} // namespace sympsica_test

#endif // SYMPSICA_TEST_GATES_ZTGATE_CONVERT_HPP
