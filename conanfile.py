from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class LuxAccel(ConanFile):
    name = "lux-accel"
    package_type = "shared-library"  # Always shared - this is what Go loads
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*", "LICENSE*"

    # Build-time dependency only - statically linked, not propagated to consumers
    requires = "lux-gpu/0.1.0"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        # Force static linking of lux-gpu
        self.options["lux-gpu/*"].fPIC = True

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", self.name)
        self.cpp_info.set_property("cmake_target_name", "lux::accel")
        self.cpp_info.set_property("cmake_find_mode", "config")
        self.cpp_info.libs = ["lux_accel"]

        # No transitive dependencies - everything is bundled
        # Go consumers only need liblux-accel.so/dylib

        # Platform libs for consumers linking this shared lib
        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["dl"]
