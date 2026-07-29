# Build Executables

## Two ways to resolve the dependencies

The dependencies come from Conan 2 either way. The difference is only when they are resolved.

Separate install, then a plain configure:

```bash
conan install . --build=missing -s build_type=Release
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake
cmake --build build -j"$(nproc)"
```

Or through [cmake-conan](https://github.com/conan-io/cmake-conan), which resolves them during the
configure step. This is what CI does:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPRIVATEER_BUILD_TESTING=ON \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=conan_provider.cmake
cmake --build build -j"$(nproc)"
```

`conan_provider.cmake` is not part of this repository; it comes from cmake-conan. `CC` and `CXX`
select the Conan profile, so set them before configuring if the default compiler is not the one
wanted.

## CMake options

* `PRIVATEER_BUILD_TESTING`
    * Build the test suites. Also compiles the test-only hooks into the library: failure-injection
      seams, crash-test kill points and internal accessors. They must not be in a packaged build,
      which is why the Conan recipe turns this off.
    * ON or OFF. Default is ON for a top-level build, OFF when the project is added as a subproject.

* `PRIVATEER_BUILD_BENCHMARKS`
    * Build the Google Benchmark binaries under `bench/`.
    * ON or OFF. Default is ON for a top-level build, OFF when added as a subproject.

Build type is the usual CMake variable. Sanitizer builds are ordinary builds with flags, and CI runs
them as RelWithDebInfo:

```bash
cmake -G Ninja -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=conan_provider.cmake \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

AddressSanitizer and ThreadSanitizer are mutually exclusive, so races need their own build. Both
want a sanitizer-friendly container or host: without `seccomp=unconfined` a ThreadSanitizer binary
aborts at startup over ASLR, which looks like flakiness.

## The benchmarks

`PRIVATEER_BUILD_BENCHMARKS=ON` builds four binaries:

* `region_bench`: the fault, commit, cleaner and snapshot paths of a whole region.
* `barrier_bench`: shapes of the durability barrier, including the io_uring arms. The engine itself
  does not use io_uring; the arms exist to price the alternative.
* `hash_bench`: block hashing throughput.
* `addressing_bench`: the per-block commit mechanics of content addressing, against writing a fresh
  file for every dirty block.

They create datastores under `TMPDIR`. Put it on the device under test. A virtual or network file
system distorts the write arms enough to invert results.

## Installing

```bash
cmake --install build --prefix <prefix>
```

This installs the static library, the headers and the CMake export set. A Conan package installs the
same and drops the export set, because consumers find the package through the generated CMakeDeps
files.
