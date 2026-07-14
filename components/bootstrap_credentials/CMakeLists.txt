idf_component_register(
  SRCS "bootstrap_credentials.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_system nvs_flash
)
