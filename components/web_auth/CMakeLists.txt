idf_component_register(
  SRCS "web_auth.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config esp_http_server esp_timer esp_system
)
