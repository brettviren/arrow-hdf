# arrow-hdf tests.

# Pure core tests (no Arrow/HDF5) — always built.
add_executable(test_address test_address.cpp)
target_link_libraries(test_address PRIVATE arrow_hdf_core)
add_test(NAME address COMMAND test_address)

add_executable(test_hierarchy test_hierarchy.cpp)
target_link_libraries(test_hierarchy PRIVATE arrow_hdf_core)
add_test(NAME hierarchy COMMAND test_hierarchy)

# Arrow<->HDF5 round-trip tests — only when HDF5 is available.
if(ARROW_HDF_HAVE_HDF5)
  add_executable(test_roundtrip test_roundtrip.cpp)
  target_link_libraries(test_roundtrip PRIVATE arrow_hdf
    Arrow::arrow_shared HDF5::HDF5 ${ARROW_HDF_RUNTIME_LIBS})
  add_test(NAME roundtrip COMMAND test_roundtrip)
endif()
