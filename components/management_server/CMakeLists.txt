# Remove macOS/iCloud conflict-copy artifacts before ESP-IDF scans assets.
set(MANAGEMENT_SERVER_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(ASSET_DIR "${MANAGEMENT_SERVER_DIR}/assets")
include("${MANAGEMENT_SERVER_DIR}/cleanup_conflict_assets.cmake")

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
    "${MANAGEMENT_SERVER_DIR}/assets/index.html"
    "${MANAGEMENT_SERVER_DIR}/assets/app.js"
    "${MANAGEMENT_SERVER_DIR}/assets/app.css"
  COMMAND npm ci
  COMMAND npm run check
  COMMAND npm test -- --run
  COMMAND npm run build
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_DIR}/webui/dist/index.html"
          "${MANAGEMENT_SERVER_DIR}/assets/index.html"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_DIR}/webui/dist/app.js"
          "${MANAGEMENT_SERVER_DIR}/assets/app.js"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_DIR}/webui/dist/app.css"
          "${MANAGEMENT_SERVER_DIR}/assets/app.css"
  COMMAND "${CMAKE_COMMAND}"
          "-DASSET_DIR=${MANAGEMENT_SERVER_DIR}/assets"
          -P "${MANAGEMENT_SERVER_DIR}/cleanup_conflict_assets.cmake"
  DEPENDS ${WEBUI_INPUTS} ${WEBUI_SOURCE_INPUTS}
  WORKING_DIRECTORY "${PROJECT_DIR}/webui"
  COMMENT "Checking and rebuilding embedded management UI"
  VERBATIM
)
add_custom_target(management_ui_assets DEPENDS
  "${MANAGEMENT_SERVER_DIR}/assets/index.html"
  "${MANAGEMENT_SERVER_DIR}/assets/app.js"
  "${MANAGEMENT_SERVER_DIR}/assets/app.css"
)
add_dependencies(${COMPONENT_LIB} management_ui_assets)
