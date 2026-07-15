idf_component_register(
  SRCS "ota_api.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config esp_http_server esp_system gate_runtime ota_manager web_auth
)
