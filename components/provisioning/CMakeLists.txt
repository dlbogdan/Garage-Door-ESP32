idf_component_register(
  SRCS "provisioning.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_event esp_http_server esp_netif esp_system esp_wifi lwip nvs_flash
)
