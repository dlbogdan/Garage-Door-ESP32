idf_component_register(
  SRCS "backup_envelope.cpp" "backup_service.cpp" "restore_staging.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_partition mbedtls nvs_flash
)
