idf_component_register(
  SRCS "setup_api.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config backup_recovery bootstrap_credentials esp_http_server esp_system freertos network_manager
)
