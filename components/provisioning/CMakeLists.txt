idf_component_register(
  SRCS "provisioning.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config bootstrap_credentials esp_http_server esp_system esp_timer gate_hardware gate_runtime homespan_bridge lwip network_manager
  EMBED_FILES "assets/index.html" "assets/app.js" "assets/app.css"
)
