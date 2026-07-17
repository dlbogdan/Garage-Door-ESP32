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
  kApplied = 4,
  kValidated = 5,
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
// A successfully written image is retained in staging and reported through
// applied so the caller can reboot before any NVS client is initialized.
esp_err_t apply_staged_restore_early(bool* applied);

// Advances an applied restore through two clean validation boots. The first
// successful boot records validated and requests one more restart; only the
// second successful boot erases the retained recovery image.
esp_err_t confirm_staged_restore(bool* restart_required);

// Rearms a retained applied/validated image when NVS or application validation
// rejects it. The next boot will rewrite it, subject to the attempt limit.
esp_err_t retry_applied_restore();

esp_err_t erase_staged_restore();

}  // namespace gate::backup
