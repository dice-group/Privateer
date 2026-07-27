import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, rmdir


class PrivateerConan(ConanFile):
    name = "privateer"
    version = "0.2.0"
    license = "MIT"
    author = "https://github.com/dice-group"
    url = "https://github.com/dice-group/Privateer"
    homepage = "https://github.com/dice-group/Privateer"
    description = ("Versioned segment storage for memory-mapped data. Blocks are mapped private, the first write "
                   "to a block is caught by a fault barrier, and a commit writes back only the dirty blocks under "
                   "content-addressed names.")
    topics = ("memory-mapped-io", "content-addressable-storage", "copy-on-write", "snapshots", "metall")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False], "build_legacy": [True, False]}
    default_options = {"fPIC": True, "build_legacy": False}
    exports = "LICENSE", "NOTICE"
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*"

    def requirements(self):
        self.requires("asio/1.38.0", transitive_headers=True)
        self.requires("blake3/1.8.5")
        self.requires("xxhash/0.8.3")
        self.requires("openssl/3.6.1")
        self.requires("rapidhash/3.0")
        self.requires("boost/1.88.0", headers=True, libs=False, transitive_headers=True,
                      options={"header_only": True})
        if self.options.build_legacy:
            self.requires("spdlog/1.17.0")

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.9.5")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        # Tests and benchmarks build by default in a top level build, which is
        # what a package build is. The package ships neither, and the test
        # hooks must stay out of the packaged library.
        tc = CMakeToolchain(self)
        tc.cache_variables["PRIVATEER_BUILD_TESTING"] = False
        tc.cache_variables["PRIVATEER_BUILD_BENCHMARKS"] = False
        tc.cache_variables["PRIVATEER_BUILD_LEGACY"] = bool(self.options.build_legacy)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        # consumers find the package through the generated CMakeDeps files, so
        # the installed export set is redundant
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        copy(self, "LICENSE", src=self.recipe_folder, dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "NOTICE", src=self.recipe_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["privateer"]
        self.cpp_info.set_property("cmake_file_name", "privateer")
        self.cpp_info.set_property("cmake_target_name", "privateer::privateer")
        # the library carries asio's single implementation translation unit, so
        # every consumer must agree on these
        self.cpp_info.defines = ["ASIO_STANDALONE", "ASIO_SEPARATE_COMPILATION", "ASIO_NO_DEPRECATED"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread"]
