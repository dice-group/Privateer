#include <privateer/mlocked.hpp>

#include <utility>

#include <sys/mman.h>

namespace privateer {

	result<mlocked_buffer> mlocked_buffer::allocate(size_t len, bool lock) {
		if (len == 0) {
			return fail(errc::invalid_argument, "mlocked_buffer::allocate");
		}
		void *const addr = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (addr == MAP_FAILED) {
			return fail_errno(errc::io_error, "mmap anonymous");
		}
		if (lock && ::mlock(addr, len) != 0) {
			auto const err = fail_errno(errc::memlock_limit_too_low, "mlock");
			::munmap(addr, len);
			return err;
		}
		mlocked_buffer buf;
		buf.addr_ = addr;
		buf.len_ = len;
		buf.locked_ = lock;
		return buf;
	}

	mlocked_buffer::mlocked_buffer(mlocked_buffer &&other) noexcept
		: addr_{std::exchange(other.addr_, nullptr)},
		  len_{std::exchange(other.len_, 0)},
		  locked_{std::exchange(other.locked_, false)} {}

	mlocked_buffer &mlocked_buffer::operator=(mlocked_buffer &&other) noexcept {
		if (this != &other) {
			this->~mlocked_buffer();
			addr_ = std::exchange(other.addr_, nullptr);
			len_ = std::exchange(other.len_, 0);
			locked_ = std::exchange(other.locked_, false);
		}
		return *this;
	}

	mlocked_buffer::~mlocked_buffer() {
		if (addr_ != nullptr) {
			// munmap releases the lock too; munlock first is not needed
			::munmap(addr_, len_);
			addr_ = nullptr;
		}
	}

}  // namespace privateer
