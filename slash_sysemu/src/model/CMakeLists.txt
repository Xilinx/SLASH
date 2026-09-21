# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

# -------- default model server (packed into the default VBIN as `vpp_sim`) --------
#
# A minimal model process that serves round-trip memory/register traffic but runs
# no compute kernels.  It is compiled from source and, together with a committed
# system map, tarred into the default VBIN the daemon uses to bootstrap a fresh
# accelerator.  ZMQ + jsoncpp come from the parent (src) scope.

add_library(slash_sysemu_model_core STATIC model_server.cpp)
target_include_directories(slash_sysemu_model_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${JSONCPP_INCLUDE_DIRS}
)
target_compile_features(slash_sysemu_model_core PUBLIC cxx_std_20)
target_compile_options(slash_sysemu_model_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(slash_sysemu_model_core PRIVATE PkgConfig::PC_ZMQ ${JSONCPP_LIBRARIES})

# The executable is packed into the VBIN as `vpp_sim`; it is not installed on its
# own (the daemon extracts it from the VBIN at runtime).
add_executable(slash_sysemu_model model_server_main.cpp)
target_link_libraries(slash_sysemu_model PRIVATE slash_sysemu_model_core)
target_compile_options(slash_sysemu_model PRIVATE -Wall -Wextra -Werror)

# -------- assemble the default VBIN --------
#
# Stage the model binary (as `vpp_sim`) and the committed system map into a
# scratch dir, then tar them.  unpack_vbin() also accepts gzip, but a plain tar
# keeps the build hermetic and the artifact reproducible.

find_program(TAR_EXECUTABLE tar REQUIRED)

set(DEFAULT_VBIN_MAP ${CMAKE_SOURCE_DIR}/data/default/system_map.xml)
set(DEFAULT_VBIN_STAGE ${CMAKE_CURRENT_BINARY_DIR}/default_vbin_stage)
set(DEFAULT_VBIN ${CMAKE_CURRENT_BINARY_DIR}/default.vbin)

add_custom_command(
  OUTPUT ${DEFAULT_VBIN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEFAULT_VBIN_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:slash_sysemu_model>
          ${DEFAULT_VBIN_STAGE}/vpp_sim
  COMMAND ${CMAKE_COMMAND} -E copy ${DEFAULT_VBIN_MAP}
          ${DEFAULT_VBIN_STAGE}/system_map.xml
  COMMAND ${TAR_EXECUTABLE} cf ${DEFAULT_VBIN}
          -C ${DEFAULT_VBIN_STAGE} vpp_sim system_map.xml
  DEPENDS slash_sysemu_model ${DEFAULT_VBIN_MAP}
  COMMENT "Assembling default VBIN (vpp_sim + system_map.xml)"
)
add_custom_target(default_vbin ALL DEPENDS ${DEFAULT_VBIN})

# Absolute path to the built artifact, exported to the parent scope so the
# daemon (compile-time default) and the tests can reference it.
set(SLASH_SYSEMU_DEFAULT_VBIN_BUILD ${DEFAULT_VBIN} PARENT_SCOPE)

# -------- install the default VBIN --------
#
# The VBIN embeds an arch-specific ELF, so it belongs under /usr/lib.  We use a
# fixed, non-multiarch subdir (mirroring vrtd's /usr/lib/vrt) so the installed
# path — which is baked into the daemon as the default — is stable across
# distros.
if(SLASH_SYSEMU_INSTALL)
  install(FILES ${DEFAULT_VBIN} DESTINATION lib/slash-sysemu)
endif()
