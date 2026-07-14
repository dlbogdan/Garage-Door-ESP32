idf_component_register(
  SRCS "app_main.cpp"
  INCLUDE_DIRS "."
  REQUIRES app_config arduino-esp32 esp_event gate_controller homespan_bridge nvs_flash provisioning
)
