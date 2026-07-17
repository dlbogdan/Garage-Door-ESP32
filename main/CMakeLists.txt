idf_component_register(
  SRCS "app_main.cpp"
  INCLUDE_DIRS "."
  REQUIRES app_config backup_recovery esp_event gate_controller gate_runtime homespan_bridge nvs_flash ota_manager provisioning
)
