idf_component_register(
  SRCS "gate_hardware.cpp" "sensor_debouncer.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config esp_driver_gpio esp_system esp_timer freertos
)
