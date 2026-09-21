include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
# Don't emit install rules for GoogleTest: it's a test-only dependency pulled
# in via FetchContent. Leaving it on causes its headers/libs/cmake files to
# land in debian/tmp during the packaging install step and generates a
# cascade of dh_missing warnings.
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of GoogleTest" FORCE)
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
add_vrt_test(graph_authoring_test graph_authoring_test.cpp)
add_vrt_test(execution_test execution_test.cpp)
add_vrt_test(authored_graph_validation_test authored_graph_validation_test.cpp)
add_vrt_test(graph_compiler_characterization_test graph_compiler_characterization_test.cpp)
add_vrt_test(graph_pass_test graph_pass_test.cpp)
add_vrt_test(graph_stage_property_test graph_stage_property_test.cpp)
add_vrt_test(render_test render_test.cpp)
add_vrt_test(semaphore_pool_test semaphore_pool_test.cpp)
add_vrt_test(rp1_bar_window_test rp1_bar_window_test.cpp)
add_vrt_test(rp1_submitter_test rp1_submitter_test.cpp)
add_vrt_test(control_lowering_test control_lowering_test.cpp)
add_vrt_test(cpu_lowering_test cpu_lowering_test.cpp)
add_vrt_test(fpga_device_test fpga_device_test.cpp)
add_vrt_test(fpga_vbin_spec_test fpga_vbin_spec_test.cpp)

# --- GPU graph tests (conditional on HIP) ---
if(VRT_HAS_GPU)
  add_executable(gpu_graph_test gpu_graph_test.hip)
  set_source_files_properties(gpu_graph_test.hip PROPERTIES
    LANGUAGE HIP
    HIP_ARCHITECTURES "${GPU_ARCH}"
  )
  target_link_libraries(gpu_graph_test PRIVATE GTest::gtest_main GTest::gmock vrt::vrt ${HIP_LIB})
  target_include_directories(gpu_graph_test PRIVATE /opt/rocm/include)
  gtest_discover_tests(gpu_graph_test)
  add_dependencies(unit_tests gpu_graph_test)
endif()

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
