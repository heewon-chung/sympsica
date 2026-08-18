#ifndef SYMPSICA_UTILS_CRASH_HPP
#define SYMPSICA_UTILS_CRASH_HPP

namespace sympsica {

// crash_point(name) — task-17-brief.md R-CRASH: a test-only kill hook.
// Reads the SYMPSICA_CRASH_AT environment variable ONCE per process
// (function-local static, C++11 magic-statics thread-safe init) and calls
// std::_Exit(137) the first time `name` matches its value; a no-op call
// (env var unset, or set to some other name) has ZERO behavior change —
// production code (Query's atomic commit, PartyState::save()) calls this at
// specific named points so a crash test can kill the process deterministically
// at any one of them via `SYMPSICA_CRASH_AT=<name>` before exec.
//
// std::_Exit (not std::exit/abort): skips atexit handlers and stdio
// flushing, matching a REAL power-loss/kill -9 crash's semantics -- nothing
// gets a chance to "clean up" the very state this hook exists to interrupt
// mid-write. Exit code 137 mirrors POSIX's 128+SIGKILL convention (used
// here as a plain marker, not an actual signal delivery).
//
// Caching note: since the read happens once per PROCESS (not per thread,
// not per call), a caller that wants an isolated crash-under-test must
// setenv() BEFORE the first crash_point() call in that process -- e.g. a
// forked child calling setenv() right after fork(), before spawning any
// threads that might reach a crash_point() call site (see
// test/protocols/kat_query.cpp's crash-test harness).
void crash_point(const char* name);

} // namespace sympsica

#endif // SYMPSICA_UTILS_CRASH_HPP
