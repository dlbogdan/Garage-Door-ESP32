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

add_executable(app_config_tests
  app_config_test.cpp
  ../../components/app_config/app_config.cpp
)
target_include_directories(app_config_tests PRIVATE
  ../../components/app_config/include
)
target_compile_features(app_config_tests PRIVATE cxx_std_17)
target_compile_options(app_config_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME app_config_tests COMMAND app_config_tests)
