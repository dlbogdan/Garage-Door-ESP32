cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT DEFINED IDF_VERSION_MAJOR OR
   NOT IDF_VERSION_MAJOR EQUAL 5 OR
   NOT IDF_VERSION_MINOR EQUAL 5 OR
   NOT IDF_VERSION_PATCH EQUAL 4)
  message(FATAL_ERROR
    "Gate Controller requires ESP-IDF v5.5.4 exactly; detected ${IDF_VERSION}")
endif()

set(GATE_BASE_VERSION "0.3.2")

if(NOT DEFINED PROJECT_VER)
  file(GLOB_RECURSE GATE_VERSION_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/main/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/webui/src/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/webui/package.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/webui/package-lock.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/webui/vite.config.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
  list(FILTER GATE_VERSION_INPUTS EXCLUDE REGEX
       "/components/management_server/assets/")
  list(SORT GATE_VERSION_INPUTS)
  set(GATE_VERSION_MATERIAL "")
  foreach(GATE_VERSION_INPUT IN LISTS GATE_VERSION_INPUTS)
    if(NOT IS_DIRECTORY "${GATE_VERSION_INPUT}")
      file(SHA256 "${GATE_VERSION_INPUT}" GATE_INPUT_HASH)
      file(RELATIVE_PATH GATE_INPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}"
           "${GATE_VERSION_INPUT}")
      string(APPEND GATE_VERSION_MATERIAL
             "${GATE_INPUT_PATH}:${GATE_INPUT_HASH}\n")
    endif()
  endforeach()
  string(SHA256 GATE_SOURCE_HASH "${GATE_VERSION_MATERIAL}")
  string(SUBSTRING "${GATE_SOURCE_HASH}" 0 8 GATE_SOURCE_FINGERPRINT)
  execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE GATE_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(GATE_GIT_REVISION STREQUAL "")
    set(GATE_GIT_REVISION "nogit")
  endif()
  set(PROJECT_VER
      "${GATE_BASE_VERSION}-dev-${GATE_GIT_REVISION}-${GATE_SOURCE_FINGERPRINT}")
endif()
message(STATUS "Gate firmware version: ${PROJECT_VER}")
project(garage_door_esp32)
