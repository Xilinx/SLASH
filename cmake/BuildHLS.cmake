# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

include_guard(GLOBAL)

function(build_hls)
  set(oneValueArgs TARGET CPP CFG DEVICE OUT_DIR)
  cmake_parse_arguments(BHL "" "${oneValueArgs}" "" ${ARGN})

  if(NOT BHL_TARGET)
    message(FATAL_ERROR "build_hls(): TARGET is required")
  endif()
  if(NOT BHL_CPP OR NOT BHL_CFG)
    message(FATAL_ERROR "build_hls(): CPP and CFG are required")
  endif()
  if(NOT BHL_DEVICE)
    message(FATAL_ERROR "build_hls(): DEVICE is required (e.g., xcv80-lsva4737-2MHP-e-S)")
  endif()

  get_filename_component(_cpp "${BHL_CPP}" REALPATH)
  get_filename_component(_cfg "${BHL_CFG}" REALPATH)

  if(NOT EXISTS "${_cpp}")
    message(FATAL_ERROR "build_hls(): CPP not found: '${_cpp}'")
  endif()
  if(NOT EXISTS "${_cfg}")
    message(FATAL_ERROR "build_hls(): CFG not found: '${_cfg}'")
  endif()

  if("${BHL_OUT_DIR}" STREQUAL "")
    set(BHL_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()

  get_filename_component(_stem "${_cpp}" NAME_WE)
  set(_build_dir "${BHL_OUT_DIR}/build_${_stem}.${BHL_DEVICE}")
  set(_component_xml "${_build_dir}/hls/impl/ip/component.xml")
  # Copy the cfg next to the build outputs so relative paths inside the cfg keep working.
  set(_cfg_local "${_build_dir}/${_stem}.cfg")
  file(MAKE_DIRECTORY "${_build_dir}")

  find_program(VPP_EXECUTABLE NAMES v++)
  if(NOT VPP_EXECUTABLE)
    message(FATAL_ERROR "build_hls(): v++ not found. Ensure Vitis is installed and v++ is on PATH.")
  endif()

  find_program(VITIS_RUN_EXECUTABLE NAMES vitis-run)
  if(NOT VITIS_RUN_EXECUTABLE)
    message(FATAL_ERROR "build_hls(): vitis-run not found. Ensure Vitis is installed and vitis-run is on PATH.")
  endif()

  add_custom_command(
    OUTPUT "${_component_xml}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_build_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cpp}" "${_build_dir}/${_stem}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cfg}" "${_cfg_local}"
    COMMAND "${VPP_EXECUTABLE}" -c --mode hls --config "${_cfg_local}" --work_dir .
    COMMAND "${VITIS_RUN_EXECUTABLE}" --mode hls --package --config "${_cfg_local}" --work_dir .
    WORKING_DIRECTORY "${_build_dir}"
    DEPENDS "${_cpp}" "${_cfg}"
    COMMENT "HLS build: ${_stem}"
    VERBATIM
  )

  add_custom_target("${BHL_TARGET}" DEPENDS "${_component_xml}")
  set_property(TARGET "${BHL_TARGET}" PROPERTY HLS_BUILD_DIR "${_build_dir}")
  set_property(TARGET "${BHL_TARGET}" PROPERTY HLS_COMPONENT_XML "${_component_xml}")
  set("${BHL_TARGET}_BUILD_DIR" "${_build_dir}" PARENT_SCOPE)
  set("${BHL_TARGET}_COMPONENT_XML" "${_component_xml}" PARENT_SCOPE)
endfunction()

function(build_hls_dir)
  set(oneValueArgs TARGET ROOT DEVICE OUT_DIR OUT_IP_REPO OUT_KERNELS)
  set(multiValueArgs KERNELS)
  cmake_parse_arguments(BHLD "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT BHLD_TARGET)
    message(FATAL_ERROR "build_hls_dir(): TARGET is required")
  endif()
  if(NOT BHLD_ROOT)
    message(FATAL_ERROR "build_hls_dir(): ROOT is required")
  endif()
  if(NOT BHLD_DEVICE)
    message(FATAL_ERROR "build_hls_dir(): DEVICE is required")
  endif()
  if(NOT BHLD_KERNELS)
    message(FATAL_ERROR "build_hls_dir(): KERNELS is required (e.g., KERNELS increment accumulate)")
  endif()

  get_filename_component(_root "${BHLD_ROOT}" REALPATH)
  if(NOT IS_DIRECTORY "${_root}")
    message(FATAL_ERROR "build_hls_dir(): ROOT is not a directory: '${_root}'")
  endif()

  if("${BHLD_OUT_DIR}" STREQUAL "")
    set(BHLD_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${BHLD_TARGET}")
  endif()
  file(MAKE_DIRECTORY "${BHLD_OUT_DIR}")

  set(_kernel_targets "")
  set(_kernel_xmls "")

  foreach(k IN LISTS BHLD_KERNELS)
    set(_cpp "${_root}/${k}.cpp")
    set(_cfg "${_root}/${k}.cfg")

    if(NOT EXISTS "${_cpp}")
      message(FATAL_ERROR "build_hls_dir(): CPP not found for kernel '${k}': '${_cpp}'")
    endif()
    if(NOT EXISTS "${_cfg}")
      message(FATAL_ERROR "build_hls_dir(): CFG not found for kernel '${k}': '${_cfg}'")
    endif()

    set(_t "${BHLD_TARGET}_${k}")
    string(REGEX REPLACE "[^A-Za-z0-9_]+" "_" _t "${_t}")

    build_hls(
      TARGET  "${_t}"
      CPP     "${_cpp}"
      CFG     "${_cfg}"
      DEVICE  "${BHLD_DEVICE}"
      OUT_DIR "${BHLD_OUT_DIR}"
    )

    list(APPEND _kernel_targets "${_t}")
    list(APPEND _kernel_xmls "${${_t}_COMPONENT_XML}")
  endforeach()

  add_custom_target("${BHLD_TARGET}" DEPENDS ${_kernel_targets})

  if(NOT "${BHLD_OUT_IP_REPO}" STREQUAL "")
    set("${BHLD_OUT_IP_REPO}" "${BHLD_OUT_DIR}" PARENT_SCOPE)
  endif()
  if(NOT "${BHLD_OUT_KERNELS}" STREQUAL "")
    set("${BHLD_OUT_KERNELS}" "${_kernel_xmls}" PARENT_SCOPE)
  endif()
endfunction()

function(build_hls_clean)
  set(oneValueArgs TARGET DEVICE ROOT)
  set(multiValueArgs EXTRA_GLOBS)
  cmake_parse_arguments(BHLC "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT BHLC_TARGET)
    message(FATAL_ERROR "build_hls_clean(): TARGET is required")
  endif()
  if(NOT BHLC_DEVICE)
    message(FATAL_ERROR "build_hls_clean(): DEVICE is required")
  endif()

  if("${BHLC_ROOT}" STREQUAL "")
    set(BHLC_ROOT "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  set(_patterns
    "${BHLC_ROOT}/build_*.${BHLC_DEVICE}"
    "${BHLC_ROOT}/*.log"
    "${BHLC_ROOT}/.Xil"
    "${BHLC_ROOT}/CMakeCache.txt"
    "${BHLC_ROOT}/CMakeFiles"
    "${BHLC_ROOT}/cmake_install.cmake"
    "${BHLC_ROOT}/Makefile"
    "${BHLC_ROOT}/*_rewrite_cfg.cmake"
    "${BHLC_ROOT}/${BHLC_TARGET}.cmake"
  )
  if(BHLC_EXTRA_GLOBS)
    list(APPEND _patterns ${BHLC_EXTRA_GLOBS})
  endif()

  set(_clean_script "${CMAKE_CURRENT_BINARY_DIR}/${BHLC_TARGET}.cmake")
  set(_script "message(STATUS \"Cleaning HLS build outputs\")\n")
  foreach(p IN LISTS _patterns)
    string(APPEND _script "file(GLOB _matches \"${p}\")\n")
    string(APPEND _script "if(_matches)\n  file(REMOVE_RECURSE \${_matches})\nendif()\n")
  endforeach()
  file(WRITE "${_clean_script}" "${_script}")

  add_custom_target("${BHLC_TARGET}"
    COMMAND "${CMAKE_COMMAND}" -P "${_clean_script}"
    COMMENT "Cleaning HLS build outputs"
  )
endfunction()
