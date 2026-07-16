# Remove macOS/iCloud conflict copies of generated UI assets.
# Canonical assets are app.js and app.css; numbered copies must never be embedded.
file(GLOB CONFLICT_ASSETS
  "${ASSET_DIR}/app [0-9]*.js"
  "${ASSET_DIR}/app [0-9]*.css"
)
if(CONFLICT_ASSETS)
  file(REMOVE ${CONFLICT_ASSETS})
endif()
