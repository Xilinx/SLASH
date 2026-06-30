add_executable(render_demo main.cpp)
target_link_libraries(render_demo PRIVATE vrt::vrt)
set_target_properties(render_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/bin)
