idf_component_register(
  SRCS "gate_runtime.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config esp_timer gate_controller gate_hardware signal_decoder
)
