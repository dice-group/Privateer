# Getting Started

Privateer is a static library plus headers under `include/privateer/`. It is not header-only: the
fault handler, the block store and the executor are compiled.

## Requirements

* Linux or macOS, x86_64 or aarch64.
* A C++23 compiler. CI builds gcc 15 and clang 21 on Linux and gcc 15 on macOS.
* CMake 3.24 or newer.
* Conan 2 for the dependencies: standalone asio, Boost headers and xxHash. GoogleTest, Google
  Benchmark and, on Linux, liburing are needed only for the tests and the benchmarks.

## Building

```bash
git clone git@github.com:dice-group/Privateer.git
cd Privateer
conan install . --build=missing -s build_type=Release
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake
cmake --build build -j"$(nproc)"
```

See [Build Executables](../advanced_build/cmake.md) for the options and for the way CI configures the
same tree.

## Using it in a project

With Conan and CMake:

```python
def requirements(self):
    self.requires("privateer/0.2.0")
```

```cmake
find_package(privateer REQUIRED)
target_link_libraries(app PRIVATE privateer::privateer)
```

The target carries what a consumer has to agree on: C++23, and asio's
`ASIO_STANDALONE`, `ASIO_SEPARATE_COMPILATION` and `ASIO_NO_DEPRECATED`, because the library holds
asio's single implementation translation unit.

## A first datastore

```cpp
#include <privateer/region.hpp>

#include <cstdio>
#include <cstring>

int main() {
    // capacity is fixed for the lifetime of the datastore; nothing is mapped yet
    auto region = privateer::region::create("datastore", 8ull << 30);
    if (!region) {
        std::fprintf(stderr, "create failed: %s\n", to_string(region.error()).c_str());
        return 1;
    }

    // make one MiB of the segment usable, then write into it
    if (auto extended = region->extend(1ull << 20); !extended) {
        std::fprintf(stderr, "extend failed: %s\n", to_string(extended.error()).c_str());
        return 1;
    }
    std::memset(region->segment(), 'p', 1ull << 20);

    // durable: every block the new recipe references is on stable storage on return
    if (auto committed = region->commit(true); !committed) {
        std::fprintf(stderr, "commit failed: %s\n", to_string(committed.error()).c_str());
        return 1;
    }
    return 0;
}
```

Reopening it later maps the same content back at a possibly different base address, so persistent
data structures inside the segment must use offsets and not raw pointers:

```cpp
auto region = privateer::region::open("datastore");                 // read-write
auto reader = privateer::region::open_read_only("datastore");        // never mutates the datastore
auto staged = region->snapshot_to("datastore.snapshot.staging");     // a sharing copy
```

Nothing throws. Every call that can fail returns `privateer::result<T>`, an alias for
`std::expected<T, privateer::error>`. See [API](../detail/api.md) for the whole surface.
