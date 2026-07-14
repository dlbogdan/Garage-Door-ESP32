cmake_minimum_required(VERSION 3.16)
project(gate_controller_host_tests LANGUAGES CXX)

enable_testing()
add_executable(gate_controller_tests
  gate_controller_test.cpp
  ../../components/gate_controller/gate_controller.cpp
)
target_include_directories(gate_controller_tests PRIVATE
  ../../components/gate_controller/include
)
target_compile_features(gate_controller_tests PRIVATE cxx_std_17)
target_compile_options(gate_controller_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME gate_controller_tests COMMAND gate_controller_tests)

