from conan import ConanFile
from conan.tools.cmake import cmake_layout


class PrivateerConan(ConanFile):
    name = "privateer"
    version = "0.2.0"
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps"
    options = {"build_legacy": [True, False]}
    default_options = {"build_legacy": False}

    def requirements(self):
        self.requires("asio/1.38.0", transitive_headers=True)
        self.requires("blake3/1.8.5")
        self.requires("xxhash/0.8.3")
        self.requires("openssl/3.6.1")
        self.requires("rapidhash/3.0")
        if self.options.build_legacy:
            self.requires("boost/1.88.0", headers=True, libs=False, options={"header_only": True})
            self.requires("spdlog/1.17.0")

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.9.5")

    def layout(self):
        cmake_layout(self)
