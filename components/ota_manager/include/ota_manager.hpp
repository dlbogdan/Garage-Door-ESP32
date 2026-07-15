#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace gate::ota {

enum class Phase {
  kIdle,
  kReceiving,
  kVerifying,
  kReadyToReboot,
  kFailed,
};

struct Status {
  Phase phase;
  const char* version;
  const char* project_name;
  const char* running_partition;
  const char* update_partition;
  std::size_t maximum_image_size;
  std::size_t total_bytes;
  std::size_t written_bytes;
  bool pending_verification;
  bool rollback_enabled;
  const char* error;
};

// Acquires OTA maintenance mode and prepares the inactive application slot.
esp_err_t begin(std::size_t content_length);
esp_err_t write(const void* data, std::size_t length);
esp_err_t finish();
void abort(const char* reason);

Status status();
bool in_progress();

// Call after all local safety-critical subsystems have started. A pending OTA
// image is confirmed only after a short stability period.
void schedule_boot_confirmation(bool local_health_ok);

const char* phase_name(Phase phase);

}  // namespace gate::ota
