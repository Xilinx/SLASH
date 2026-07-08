# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

include(GoogleTest)

add_custom_target(unit_tests)

macro(add_slash_test test_name test_source)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main GTest::gmock slash::slash)
  set_target_properties(${test_name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
  )
  gtest_discover_tests(${test_name})
  add_dependencies(unit_tests ${test_name})
endmacro()

add_slash_test(ctldev_test ctldev_test.cpp)
add_slash_test(hotplug_test hotplug_test.cpp)
add_slash_test(qdma_test qdma_test.cpp)

# sock_transport_test needs the private src/ include directory for sock_transport.h.
add_slash_test(sock_transport_test sock_transport_test.cpp)
target_include_directories(sock_transport_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# sysemu_test_server is a reusable in-process daemon stub shared by multiple test targets.
add_library(sysemu_test_server OBJECT sysemu_test_server.cpp)
target_link_libraries(sysemu_test_server PUBLIC slash::slash)
target_include_directories(sysemu_test_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(sysemu_test_server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

# ctldev socket-transport tests.
add_slash_test(ctldev_sysemu_test ctldev_sysemu_test.cpp)
target_link_libraries(ctldev_sysemu_test PRIVATE sysemu_test_server)
target_include_directories(ctldev_sysemu_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# qdma socket-transport tests.
add_slash_test(qdma_sysemu_test qdma_sysemu_test.cpp)
target_link_libraries(qdma_sysemu_test PRIVATE sysemu_test_server)
target_include_directories(qdma_sysemu_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# hotplug socket-transport tests.
add_slash_test(hotplug_sysemu_test hotplug_sysemu_test.cpp)
target_link_libraries(hotplug_sysemu_test PRIVATE sysemu_test_server)
target_include_directories(hotplug_sysemu_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# BYO-daemon E2E tests.  Always compiled; all tests GTEST_SKIP() when
# SLASH_E2E_CTL / SLASH_E2E_QDMA / SLASH_E2E_HOTPLUG are unset, so ctest
# stays green with no daemon.  The suite NEVER spawns the daemon itself.
add_slash_test(libslash_e2e_test libslash_e2e_test.cpp)
