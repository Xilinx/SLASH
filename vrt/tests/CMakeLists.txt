include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

include(GoogleTest)

add_custom_target(unit_tests)

macro(add_vrt_test test_name test_source)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main GTest::gmock vrt::vrt)
  gtest_discover_tests(${test_name})
  add_dependencies(unit_tests ${test_name})
endmacro()

add_vrt_test(register_test register_test.cpp)
add_vrt_test(qdma_connection_test qdma_connection_test.cpp)
add_vrt_test(logger_test logger_test.cpp)
add_vrt_test(utilization_data_test utilization_data_test.cpp)
add_vrt_test(filesystem_cache_test filesystem_cache_test.cpp)
add_vrt_test(xml_parser_test xml_parser_test.cpp)
add_vrt_test(utilization_parser_test utilization_parser_test.cpp)
add_vrt_test(kernel_test kernel_test.cpp)
add_vrt_test(vrtbin_test vrtbin_test.cpp)

# --- Stub VBIN generation ---
set(STUB_VBIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stub_vbin)

set(STUB_EMU_VBIN ${CMAKE_CURRENT_BINARY_DIR}/stub_emu.vbin)
add_custom_command(
  OUTPUT ${STUB_EMU_VBIN}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging
  COMMAND ${CMAKE_COMMAND} -E copy
    ${STUB_VBIN_DIR}/system_map_emu.xml
    ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging/system_map.xml
  COMMAND ${CMAKE_COMMAND} -E copy
    ${STUB_VBIN_DIR}/emu_manifest.json
    ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging/emu_manifest.json
  COMMAND ${CMAKE_COMMAND} -E copy
    ${STUB_VBIN_DIR}/vrt_stub_server.py
    ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging/vpp_emu
  COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging/vpp_emu
  COMMAND tar cf ${STUB_EMU_VBIN}
    -C ${CMAKE_CURRENT_BINARY_DIR}/stub_emu_staging .
  DEPENDS
    ${STUB_VBIN_DIR}/system_map_emu.xml
    ${STUB_VBIN_DIR}/emu_manifest.json
    ${STUB_VBIN_DIR}/vrt_stub_server.py
  COMMENT "Creating emulation stub VBIN"
)

set(STUB_SIM_VBIN ${CMAKE_CURRENT_BINARY_DIR}/stub_sim.vbin)
add_custom_command(
  OUTPUT ${STUB_SIM_VBIN}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/stub_sim_staging
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/stub_sim_staging
  COMMAND ${CMAKE_COMMAND} -E copy
    ${STUB_VBIN_DIR}/system_map_sim.xml
    ${CMAKE_CURRENT_BINARY_DIR}/stub_sim_staging/system_map.xml
  COMMAND ${CMAKE_COMMAND} -E copy
    ${STUB_VBIN_DIR}/vrt_stub_server.py
    ${CMAKE_CURRENT_BINARY_DIR}/stub_sim_staging/vpp_sim
  COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/stub_sim_staging/vpp_sim
  COMMAND tar cf ${STUB_SIM_VBIN}
    -C ${CMAKE_CURRENT_BINARY_DIR}/stub_sim_staging .
  DEPENDS
    ${STUB_VBIN_DIR}/system_map_sim.xml
    ${STUB_VBIN_DIR}/vrt_stub_server.py
  COMMENT "Creating simulation stub VBIN"
)

add_custom_target(stub_vbins DEPENDS ${STUB_EMU_VBIN} ${STUB_SIM_VBIN})

macro(add_vrt_vbin_test test_name test_source)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main GTest::gmock vrt::vrt)
  target_compile_definitions(${test_name} PRIVATE
    STUB_EMU_VBIN_PATH="${STUB_EMU_VBIN}"
    STUB_SIM_VBIN_PATH="${STUB_SIM_VBIN}")
  add_dependencies(${test_name} stub_vbins)
  gtest_discover_tests(${test_name})
  add_dependencies(unit_tests ${test_name})
endmacro()

add_vrt_vbin_test(vrtbin_integration_test vrtbin_integration_test.cpp)
add_vrt_vbin_test(device_test device_test.cpp)
