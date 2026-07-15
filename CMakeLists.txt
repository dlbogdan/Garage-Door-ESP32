cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT DEFINED IDF_VERSION_MAJOR OR
   NOT IDF_VERSION_MAJOR EQUAL 5 OR
   NOT IDF_VERSION_MINOR EQUAL 5 OR
   NOT IDF_VERSION_PATCH EQUAL 4)
  message(FATAL_ERROR
    "Gate Controller requires ESP-IDF v5.5.4 exactly; detected ${IDF_VERSION}")
endif()

set(PROJECT_VER "0.3.2")
project(garage_door_esp32)
