from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools.files import copy
from os.path import join

required_conan_version = ">=1.60.0"

class HomeBlocksConan(ConanFile):
    name = "homeblocks"
    version = "2.1.2"
    homepage = "https://github.com/eBay/HomeBlocks"
    description = "Block Store built on HomeStore"
    topics = ("ebay")
    url = "https://github.com/eBay/HomeBlocks"
    license = "Apache-2.0"

    settings = "arch", "os", "compiler", "build_type"

    options = {
                "shared": ['True', 'False'],
                "fPIC": ['True', 'False'],
                "coverage": ['True', 'False'],
                "sanitize": ['True', 'False'],
                "fixed_index": [True, False],
              }
    default_options = {
                'shared': False,
                'fPIC': True,
                'coverage': False,
                'sanitize': False,
                'fixed_index': True,
            }

    exports_sources = ("CMakeLists.txt", "cmake/*", "src/*", "LICENSE")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        if self.options.coverage and self.options.sanitize:
            raise ConanInvalidConfiguration("Sanitizer does not work with Code Coverage!")

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def requirements(self):
        self.requires("homestore/[~6.18]@oss/master", transitive_headers=True)
        self.requires("iomgr/[^11.3]@oss/master", transitive_headers=True)
        self.requires("sisl/[^12.2]@oss/master", transitive_headers=True)
        self.requires("lz4/1.9.4", override=True)

    def validate(self):
        if self.info.settings.compiler.cppstd:
            check_min_cppstd(self, 20)

    def layout(self):
        self.folders.source = "."
        if self.options.get_safe("sanitize"):
            self.folders.build = join("build", "Sanitized")
        elif self.options.get_safe("coverage"):
            self.folders.build = join("build", "Coverage")
        else:
            self.folders.build = join("build", str(self.settings.build_type))
        self.folders.generators = join(self.folders.build, "generators")

        self.cpp.source.components["homestore"].includedirs = ["src/include"]

        self.cpp.build.components["homestore"].libdirs = ["src/lib/volume"]

        self.cpp.package.components["homestore"].libs = ["homeblocks_volume"]
        self.cpp.package.includedirs = ["include"] # includedirs is already set to 'include' by
        self.cpp.package.libdirs = ["lib"]

    def generate(self):
        # This generates "conan_toolchain.cmake" in self.generators_folder
        tc = CMakeToolchain(self)
        tc.variables["CONAN_CMAKE_SILENT_OUTPUT"] = "ON"
        tc.variables['CMAKE_EXPORT_COMPILE_COMMANDS'] = 'ON'
        tc.variables["CTEST_OUTPUT_ON_FAILURE"] = "ON"
        tc.variables["MEMORY_SANITIZER_ON"] = "OFF"
        tc.variables["CODE_COVERAGE"] = "OFF"
        tc.variables["PROJECT_VERSION"] = self.version
        tc.variables["USE_FIXED_INDEX"] = "ON" if self.options.fixed_index else "OFF"
        if self.settings.build_type == "Debug":
            if self.options.get_safe("coverage"):
                tc.variables['CODE_COVERAGE'] = 'ON'
            elif self.options.get_safe("sanitize"):
                tc.variables['MEMORY_SANITIZER_ON'] = 'ON'
        tc.generate()

        # This generates "boost-config.cmake" and "grpc-config.cmake" etc in self.generators_folder
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if not self.conf.get("tools.build:skip_test", default=False):
            cmake.test()

    def package(self):
        lib_dir = join(self.package_folder, "lib")
        copy(self, "LICENSE", self.source_folder, join(self.package_folder, "licenses"), keep_path=False)
        copy(self, "*.lib", self.build_folder, lib_dir, keep_path=False)
        copy(self, "*.a", self.build_folder, lib_dir, keep_path=False)
        copy(self, "*.dylib*", self.build_folder, lib_dir, keep_path=False)
        copy(self, "*.dll*", self.build_folder, join(self.package_folder, "bin"), keep_path=False)
        copy(self, "*.so*", self.build_folder, lib_dir, keep_path=False)
        copy(self, "*", join(self.source_folder, "src", "flip", "client", "python"), join(self.package_folder, "bindings", "flip", "python"), keep_path=False)

        copy(self, "*.h*", join(self.source_folder, "src", "include"), join(self.package_folder, "include"), keep_path=True)

    def package_info(self):
        self.cpp_info.components["homestore"].libs = ["homeblocks_volume"]
        self.cpp_info.components["homestore"].requires = ["homestore::homestore", "iomgr::iomgr", "sisl::sisl"]
        self.cpp_info.components["homeblocks"].requires = ["homestore"]

        if self.settings.os == "Linux":
            #self.cpp_info.components["homestore"].system_libs.append("pthread")
            self.cpp_info.components["memory"].system_libs.append("pthread")
        if  self.options.sanitize:
            self.cpp_info.components["memory"].sharedlinkflags.append("-fsanitize=address")
            self.cpp_info.components["memory"].exelinkflags.append("-fsanitize=address")
            self.cpp_info.components["memory"].sharedlinkflags.append("-fsanitize=undefined")
            self.cpp_info.components["memory"].exelinkflags.append("-fsanitize=undefined")
