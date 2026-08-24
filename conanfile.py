import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools.files import copy, rmdir

required_conan_version = ">=2.0"


class WaSafeConan(ConanFile):
    name = "wasafe"
    version = "0.1.0"

    url = "https://github.com/gikon3/wasafe"
    homepage = "https://github.com/gikon3/wasafe"
    description = "Waveform database backend"
    license = "Apache-2.0"
    topics = ("waveform", "systemverilog", "eda")

    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"

    implements = ["auto_shared_fpic"]

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_zstd": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_zstd": True,
    }

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
        "tests/*",
        "examples/*",
        "LICENSE*",
    )

    @property
    def _skip_test(self):
        return self.conf.get("tools.build:skip_test", default=False, check_type=bool)

    def layout(self):
        cmake_layout(self)
        self.cpp.source.includedirs = ["include"]
        self.cpp.build.includedirs = ["src/export"]
        self.cpp.build.bindirs = ["bin"]
        self.cpp.build.libdirs = ["lib"]

    def requirements(self):
        if self.options.with_zstd:
            self.requires("zstd/1.5.6")

    def build_requirements(self):
        if not self._skip_test:
            self.test_requires("gtest/1.17.0")

    def validate(self):
        check_min_cppstd(self, "23")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["WASAFE_WITH_ZSTD"] = bool(self.options.with_zstd)
        tc.cache_variables["WASAFE_VERSION"] = self.version
        tc.cache_variables["WASAFE_DESCRIPTION"] = self.description
        tc.cache_variables["WASAFE_HOMEPAGE_URL"] = self.homepage
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        cmake.test()

    def package(self):
        copy(self, "LICENSE*", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        CMake(self).install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.libs = ["wasafe"]
        self.cpp_info.set_property("cmake_target_name", "wasafe::wasafe")
        self.cpp_info.set_property("cmake_file_name", "wasafe")
        if not self.options.shared:
            self.cpp_info.defines.append("WASAFE_STATIC_DEFINE")
