set(HOMESPAN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/HomeSpan")

idf_component_register(
  SRCS
    "${HOMESPAN_ROOT}/src/HAP.cpp"
    "${HOMESPAN_ROOT}/src/HKDF.cpp"
    "${HOMESPAN_ROOT}/src/HomeSpan.cpp"
    "${HOMESPAN_ROOT}/src/Network.cpp"
    "${HOMESPAN_ROOT}/src/SRP.cpp"
    "${HOMESPAN_ROOT}/src/TLV8.cpp"
    "${HOMESPAN_ROOT}/src/Utils.cpp"
    "${HOMESPAN_ROOT}/src/src/extras/Blinker.cpp"
  INCLUDE_DIRS "${HOMESPAN_ROOT}/src"
  REQUIRES
    app_update
    arduino-esp32
    esp_app_format
    esp_timer
    esp_wifi
    libsodium
    mbedtls
    mdns
    nvs_flash
    spi_flash
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC HOMESPAN_IDF_COMPONENT=1)
