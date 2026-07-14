idf_component_register(
  SRCS "app_config.cpp" "config_repository.cpp"
  INCLUDE_DIRS "include"
  REQUIRES nvs_flash
)
