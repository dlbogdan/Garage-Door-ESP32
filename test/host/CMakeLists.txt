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

add_executable(sensor_debouncer_tests
  sensor_debouncer_test.cpp
  ../../components/gate_hardware/sensor_debouncer.cpp
)
target_include_directories(sensor_debouncer_tests PRIVATE
  ../../components/gate_hardware/include
)
target_compile_features(sensor_debouncer_tests PRIVATE cxx_std_17)
target_compile_options(sensor_debouncer_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME sensor_debouncer_tests COMMAND sensor_debouncer_tests)

add_executable(relay_pulse_guard_tests
  relay_pulse_guard_test.cpp
  ../../components/gate_controller/relay_pulse_guard.cpp
)
target_include_directories(relay_pulse_guard_tests PRIVATE
  ../../components/gate_controller/include
)
target_compile_features(relay_pulse_guard_tests PRIVATE cxx_std_17)
target_compile_options(relay_pulse_guard_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME relay_pulse_guard_tests COMMAND relay_pulse_guard_tests)

add_executable(homekit_projection_tests
  homekit_projection_test.cpp
  ../../components/homespan_bridge/homekit_projection.cpp
)
target_include_directories(homekit_projection_tests PRIVATE
  ../../components/gate_controller/include
  ../../components/homespan_bridge/include
)
target_compile_features(homekit_projection_tests PRIVATE cxx_std_17)
target_compile_options(homekit_projection_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME homekit_projection_tests COMMAND homekit_projection_tests)
