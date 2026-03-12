cmake_minimum_required(VERSION 3.20)
project(lux-accel VERSION 0.1.0 LANGUAGES CXX)

# lux-accel is THE shared library for Go/FFI consumers
# It statically links lux-gpu core and exports only C ABI

find_package(lux-gpu CONFIG REQUIRED)

# Build as SHARED library
add_library(luxaccel SHARED
  src/session.cpp
  src/tensor.cpp
  src/ml.cpp
  src/crypto.cpp
  src/zk.cpp
  src/lattice.cpp
  src/fhe.cpp
  src/dex.cpp
  src/c_api.cpp
)

add_library(lux::accel ALIAS luxaccel)

target_compile_features(luxaccel PUBLIC cxx_std_23)

# Building the library - export symbols
target_compile_definitions(luxaccel PRIVATE LUX_ACCEL_BUILDING)

target_include_directories(luxaccel
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link gpu core PRIVATELY (statically embedded)
# The static library symbols are absorbed into luxaccel.so
target_link_libraries(luxaccel
  PRIVATE lux::lux_gpu_core
)

# Platform libraries for dlopen (backend plugin loading)
if(UNIX AND NOT APPLE)
  target_link_libraries(luxaccel PRIVATE dl)
endif()

# Hide all symbols except those explicitly exported
set_target_properties(luxaccel PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  # Version info for soname
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
)

# macOS: ensure symbols are resolved at load time
if(APPLE)
  set_target_properties(luxaccel PROPERTIES
    MACOSX_RPATH ON
    INSTALL_RPATH "@loader_path"
  )
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install shared library
install(TARGETS luxaccel
  EXPORT lux-accel-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install only the C API header (not internal C++ headers)
install(FILES
  include/lux/accel/c_api.h
  include/lux/accel/backend_api.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lux/accel
)

install(EXPORT lux-accel-targets
  NAMESPACE lux::
  FILE lux-accelTargets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lux-accel
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux-accelConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/lux-accelConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lux-accel
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/lux-accelConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ExactVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/lux-accelConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/lux-accelConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lux-accel
)
