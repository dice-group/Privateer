#ifndef PRIVATEER_MLOCKED_HPP
#define PRIVATEER_MLOCKED_HPP

// Mlocked memory for everything the fault handler dereferences: registry
// tables, region records, state arrays, and alternate signal stacks. The
// fault signal is masked while the handler runs, so a page fault on the
// handler's own data (a reclaimed page) would kill the process; mlock
// removes that possibility.

#include <privateer/error.hpp>

#include <cstddef>

namespace privateer {

	// An anonymous mapping, mlocked on allocation, unmapped on destruction.
	// With lock false the memory is allocated but not locked: the override
	// for swapless deployments, where anonymous pages are never reclaimed.
	struct mlocked_buffer {
		static result<mlocked_buffer> allocate(size_t len, bool lock = true);

		constexpr mlocked_buffer() = default;
		mlocked_buffer(mlocked_buffer &&other) noexcept;
		mlocked_buffer &operator=(mlocked_buffer &&other) noexcept;
		mlocked_buffer(mlocked_buffer const &) = delete;
		mlocked_buffer &operator=(mlocked_buffer const &) = delete;
		~mlocked_buffer();

		[[nodiscard]] void *addr() const noexcept { return addr_; }
		[[nodiscard]] size_t size() const noexcept { return len_; }
		[[nodiscard]] bool locked() const noexcept { return locked_; }

	private:
		void *addr_ = nullptr;
		size_t len_ = 0;
		bool locked_ = false;
	};

}  // namespace privateer

#endif  // PRIVATEER_MLOCKED_HPP
