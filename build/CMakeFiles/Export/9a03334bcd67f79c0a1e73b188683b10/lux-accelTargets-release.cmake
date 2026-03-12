#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "lux::luxaccel" for configuration "Release"
set_property(TARGET lux::luxaccel APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(lux::luxaccel PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libluxaccel.0.1.0.dylib"
  IMPORTED_SONAME_RELEASE "@rpath/libluxaccel.0.dylib"
  )

list(APPEND _cmake_import_check_targets lux::luxaccel )
list(APPEND _cmake_import_check_files_for_lux::luxaccel "${_IMPORT_PREFIX}/lib/libluxaccel.0.1.0.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
