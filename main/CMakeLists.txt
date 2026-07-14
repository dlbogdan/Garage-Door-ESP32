idf_component_register(
  SRCS "app_main.cpp"
  INCLUDE_DIRS "."
  REQUIRES app_config esp_event gate_controller gate_hardware homespan_bridge nvs_flash provisioning
)
