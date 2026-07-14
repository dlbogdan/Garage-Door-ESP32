idf_component_register(
  SRCS "app_config.cpp" "config_repository.cpp" "password.cpp"
  INCLUDE_DIRS "include"
  REQUIRES mbedtls nvs_flash
)
