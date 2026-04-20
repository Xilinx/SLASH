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
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main vrt::vrt)
  gtest_discover_tests(${test_name})
  add_dependencies(unit_tests ${test_name})
endmacro()

add_vrt_test(hello_test hello_test.cpp)
add_vrt_test(register_test register_test.cpp)
add_vrt_test(qdma_connection_test qdma_connection_test.cpp)
add_vrt_test(logger_test logger_test.cpp)
add_vrt_test(utilization_data_test utilization_data_test.cpp)
add_vrt_test(filesystem_cache_test filesystem_cache_test.cpp)
add_vrt_test(xml_parser_test xml_parser_test.cpp)
add_vrt_test(utilization_parser_test utilization_parser_test.cpp)
add_vrt_test(kernel_test kernel_test.cpp)
add_vrt_test(vrtbin_test vrtbin_test.cpp)
