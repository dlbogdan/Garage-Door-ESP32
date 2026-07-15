idf_component_register(
  SRCS "provisioning.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config bootstrap_credentials captive_dns homespan_bridge management_server network_manager
)
