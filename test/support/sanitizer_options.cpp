// Sanitizer defaults for the test binaries. One definition per binary, so
// this lives in its own translation unit rather than in a header.

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PRIVATEER_TEST_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define PRIVATEER_TEST_TSAN 1
#endif

#ifdef PRIVATEER_TEST_TSAN

// The crash harness forks, and one of its children starts a thread: the test
// that parks a writer at the hard watermark and then closes the region needs
// a second thread to do the closing. Once an earlier test in the same process
// has opened a region, the executor threads are alive, so the fork is a
// multi-threaded fork and TSan kills a child that creates a thread. The
// children are short lived and expected to die from the fault they provoke,
// and a child that hangs instead hits the per-test timeout, so the guard is
// turned off here.
//
// TSan must also not own the fault-signal dispositions: the write barrier
// installs a SIGSEGV handler (SIGBUS as well on Darwin) and consumes
// protection faults through it, and the crash tests expect an unhandled fault
// to kill the child. With TSan's own handlers in place the barrier never sees
// the fault and those tests fail for a reason that has nothing to do with
// threads.
//
// These are defaults: each flag in TSAN_OPTIONS still overrides what this
// returns.
extern "C" char const *__tsan_default_options() {
	return "die_after_fork=0 handle_segv=0 handle_sigbus=0";
}

#endif
