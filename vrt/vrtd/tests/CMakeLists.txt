include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

include(GoogleTest)

add_custom_target(unit_tests)

macro(add_vrtd_test test_name test_source)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main GTest::gmock vrtd_core)
  set_target_properties(${test_name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
  )
  gtest_discover_tests(${test_name})
  add_dependencies(unit_tests ${test_name})
endmacro()

add_vrtd_test(allocator_test allocator_test.cpp)
add_vrtd_test(array_test array_test.cpp)
add_vrtd_test(utils_test utils_test.cpp)
add_vrtd_test(hotplug_test hotplug_test.cpp)
add_vrtd_test(config_test config_test.cpp)
add_vrtd_test(auth_test auth_test.cpp)
add_vrtd_test(buffer_test buffer_test.cpp)
add_vrtd_test(design_writer_test design_writer_test.cpp)
add_vrtd_test(device_test device_test.cpp)
