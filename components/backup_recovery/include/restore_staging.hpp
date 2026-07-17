#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esp_err.h"

namespace gate::backup {

enum class RestoreState : std::uint32_t {
  kReady = 1,
  kApplying = 2,
  kFailed = 3,
};

struct RestoreStatus final {
  bool present{false};
  RestoreState state{RestoreState::kFailed};
  std::uint32_t sequence{0};
  std::uint32_t attempts{0};
  std::uint32_t image_size{0};
};

// Writes and verifies a complete decrypted NVS image before atomically
// publishing a ready manifest. The staging partition contains secrets and is
// erased on every new staging attempt and after successful application.
esp_err_t stage_restore_image(const std::vector<std::uint8_t>& nvs_image);

esp_err_t restore_status(RestoreStatus* status);

// Runs before nvs_flash_init(). A ready job is marked applying before NVS is
// erased. Interrupted applications are retried, with a hard attempt limit.
esp_err_t apply_staged_restore_early();

esp_err_t erase_staged_restore();

}  // namespace gate::backup
