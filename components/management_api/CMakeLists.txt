idf_component_register(
  SRCS "management_api.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config arduino-esp32 backup_recovery bootstrap_credentials esp_http_server esp_system freertos gate_hardware gate_profile gate_runtime homespan_bridge web_auth
)
