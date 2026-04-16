include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(
  hello_test
  hello_test.cpp
)
target_link_libraries(
  hello_test
  PUBLIC
  GTest::gtest_main
  vrt::vrt
)

include(GoogleTest)
gtest_discover_tests(hello_test)