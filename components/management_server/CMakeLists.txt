# Remove macOS conflict-copy artifacts before ESP-IDF scans component assets.
# Only the canonical, explicitly embedded app.js/app.css files are valid.
file(REMOVE
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/app 2.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/app 2.css"
)

idf_component_register(
  SRCS "management_server.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_config bootstrap_credentials esp_http_server esp_system freertos management_api ota_api setup_api
  EMBED_FILES "assets/index.html" "assets/app.js" "assets/app.css"
)

set(WEBUI_INPUTS
  "${PROJECT_DIR}/webui/index.html"
  "${PROJECT_DIR}/webui/package.json"
  "${PROJECT_DIR}/webui/package-lock.json"
  "${PROJECT_DIR}/webui/vite.config.js"
)
file(GLOB_RECURSE WEBUI_SOURCE_INPUTS CONFIGURE_DEPENDS
  "${PROJECT_DIR}/webui/src/*"
)
add_custom_command(
  OUTPUT
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.css"
  COMMAND npm ci
  COMMAND npm run check
  COMMAND npm test -- --run
  COMMAND npm run build
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_DIR}/webui/dist/index.html"
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/index.html"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_DIR}/webui/dist/app.js"
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.js"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_DIR}/webui/dist/app.css"
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.css"
  COMMAND "${CMAKE_COMMAND}" -E rm -f
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/app 2.js"
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/app 2.css"
  DEPENDS ${WEBUI_INPUTS} ${WEBUI_SOURCE_INPUTS}
  WORKING_DIRECTORY "${PROJECT_DIR}/webui"
  COMMENT "Checking and rebuilding embedded management UI"
  VERBATIM
)
add_custom_target(management_ui_assets DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/index.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.css"
)
add_dependencies(${COMPONENT_LIB} management_ui_assets)
