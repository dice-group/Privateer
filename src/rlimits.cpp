#include <privateer/rlimits.hpp>

#include <algorithm>

#include <sys/resource.h>

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

namespace privateer {

	result<> ensure_memlock_limit(size_t at_least) noexcept {
		rlimit lim{};
		if (::getrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
			return fail_errno(errc::io_error, "getrlimit RLIMIT_MEMLOCK");
		}
		if (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur >= at_least) {
			return {};
		}
		if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < at_least) {
			return fail(errc::memlock_limit_too_low, "RLIMIT_MEMLOCK hard limit");
		}
		lim.rlim_cur = at_least;
		if (::setrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
			return fail_errno(errc::memlock_limit_too_low, "setrlimit RLIMIT_MEMLOCK");
		}
		return {};
	}

	result<> raise_nofile_limit() noexcept {
#ifdef __APPLE__
		rlimit lim{};
		if (::getrlimit(RLIMIT_NOFILE, &lim) != 0) {
			return fail_errno(errc::io_error, "getrlimit RLIMIT_NOFILE");
		}
		// the kernel rejects soft limits above OPEN_MAX even under an
		// unlimited hard limit
		rlim_t target = static_cast<rlim_t>(OPEN_MAX);
		if (lim.rlim_max != RLIM_INFINITY) {
			target = std::min(target, lim.rlim_max);
		}
		if (lim.rlim_cur >= target) {
			return {};
		}
		lim.rlim_cur = target;
		if (::setrlimit(RLIMIT_NOFILE, &lim) != 0) {
			return fail_errno(errc::io_error, "setrlimit RLIMIT_NOFILE");
		}
#endif
		return {};
	}

}  // namespace privateer
