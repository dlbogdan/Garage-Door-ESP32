idf_component_register(
  SRCS "provisioning.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config esp_event esp_http_server esp_netif esp_system esp_timer esp_wifi gate_hardware lwip nvs_flash
  EMBED_FILES "assets/index.html" "assets/app.js" "assets/app.css"
)
