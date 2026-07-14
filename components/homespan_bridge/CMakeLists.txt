idf_component_register(
  SRCS "homespan_compatibility.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config arduino-esp32 gate_controller gate_runtime homespan
)
