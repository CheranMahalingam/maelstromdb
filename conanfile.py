from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class MaelstromDB(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    options = {"test": [True, False]}
    default_options = {"test": True}

    def requirements(self):
        self.requires("grpc/1.54.3")
        self.requires("protobuf/3.21.12")

    def build_requirements(self):
        self.build_requires("cmake/3.27.7")
        self.build_requires("protobuf/3.21.12")
        self.build_requires("ninja/1.11.1")
        self.test_requires("gtest/1.14.0")
        self.test_requires("benchmark/1.8.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self, generator="Ninja")
        if not self.options.test:
            tc.variables["MAELSTROMDB_BUILD_TESTS"] = "OFF"
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
