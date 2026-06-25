cmake_minimum_required(VERSION 3.25)
project(arrow_hdf VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Suppress spurious warnings in GCC 14/15 headers (fatal under -Werror below).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14.1")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "15")
      add_compile_options(-Wno-stringop-overflow)
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16")
      add_compile_options(-Wno-array-bounds)
    endif()
  endif()
endif()

add_compile_options(-Wall -Wpedantic -Werror)

# ---------------------------------------------------------------------------
# arrow_hdf_core — pure, dependency-free.
#
# The Phlex-neutral Address type, path escaping, and the file-hierarchy
# descriptor.  No Arrow, no HDF5: always built and tested.
# ---------------------------------------------------------------------------
add_library(arrow_hdf_core SHARED
  arrow_hdf/Address.cpp
  arrow_hdf/Hierarchy.cpp
)
target_include_directories(arrow_hdf_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------
# arrow_hdf — generic Arrow <-> HDF5 serialization.
#
# Needs Apache Arrow + HDF5 (C API) only.  When HDF5 is absent (e.g. not yet in
# the Spack view) this target and its tests are skipped, but the pure core
# still builds and tests.
# ---------------------------------------------------------------------------
find_package(Arrow CONFIG QUIET)
find_package(HDF5 QUIET COMPONENTS C)

# When using a Spack view, shared libs may be built with a newer GCC runtime
# than the system compiler; link the view's libstdc++ explicitly for targets
# that pull in Arrow.  (Same pattern as wire-cell-arrow.)
# Resolve THIS compiler's libstdc++ directly, independent of CMAKE_PREFIX_PATH
# order. A Spack build can place an older gcc-runtime earlier on the path; the
# previous prefix scan grabbed that, linking gcc@15 objects against gcc@11
# libstdc++ (missing GLIBCXX_3.4.31/.32). The compiler always yields its own.
execute_process(
  COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so
  OUTPUT_VARIABLE _stdc_real OUTPUT_STRIP_TRAILING_WHITESPACE)
file(REAL_PATH "${_stdc_real}" _stdc_real)
get_filename_component(_stdc_dir "${_stdc_real}" DIRECTORY)
set(ARROW_HDF_RUNTIME_LIBS "${_stdc_real}")
link_directories(BEFORE "${_stdc_dir}")
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-rpath,${_stdc_dir}")

set(ARROW_HDF_HAVE_HDF5 FALSE)
if(Arrow_FOUND AND HDF5_FOUND)
  set(ARROW_HDF_HAVE_HDF5 TRUE)
  add_library(arrow_hdf SHARED
    arrow_hdf/Hdf5File.cpp
  )
  target_link_libraries(arrow_hdf
    PUBLIC
      arrow_hdf_core
      Arrow::arrow_shared
      HDF5::HDF5
  )
  target_include_directories(arrow_hdf
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:include>
  )
else()
  message(STATUS "arrow-hdf: Arrow and/or HDF5 not found "
                 "(Arrow_FOUND=${Arrow_FOUND} HDF5_FOUND=${HDF5_FOUND}); "
                 "building arrow_hdf_core only. The HDF5 serialization library "
                 "and its round-trip tests are skipped until HDF5 is available.")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(test)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS arrow_hdf_core
  EXPORT arrow_hdfTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
if(ARROW_HDF_HAVE_HDF5)
  install(TARGETS arrow_hdf
    EXPORT arrow_hdfTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  )
endif()
install(DIRECTORY arrow_hdf/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/arrow_hdf"
  FILES_MATCHING PATTERN "*.hpp"
)

# CMake package config so downstream `find_package(arrow_hdf CONFIG)` works.
install(EXPORT arrow_hdfTargets
  FILE arrow_hdfTargets.cmake
  NAMESPACE arrow_hdf::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/arrow_hdf"
)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/arrow_hdfConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/arrow_hdfConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/arrow_hdfConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/arrow_hdf"
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/arrow_hdfConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/arrow_hdfConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/arrow_hdf"
)
